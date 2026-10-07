// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// GGPO for lobby battles of the Z game (DEFAULT_OPTIONS) is on by default: setting DEV9/Eth ZdxsvGgpo.
// ZDXSV_GGPO replaces it (GgpoOnVmInitialize); ZDXSV_GGPO=0 = off.
// ZDXSV_GGPO="key=value,...": a GGPO session in a running game. Synctest by default:
// every frame is saved, and every `check` frames GGPO loads the frame `check` back, reruns the
// frames with the same inputs and compares the state checksums (hash=).
// With net=1 a battle of players= peers instead (see NetInput): the session starts when the game
// arms its battle sock, every peer runs its own position, no state hashes.
//   net=1        GGPO battle session (zdxsv/rbk.sh, m4relay.sh)
//   players=4    peers (2..4); GGPO player = battle position + 1
//   port=7001    UDP port of position 0; position p listens on port + p, peers on host= (default 127.0.0.1)
//   relay=R      remote p is at R + 8 * me + p (zdxsv/udprelay.py per pair)
//   lobby=1      battles from the zdxsv lobby: platform info announces ggpo=port, the lobby's battle
//                info gives players and peer addresses; listen on port itself. A peer without GGPO:
//                the battle stays on the battle server (TCP). A peer that did not answer the ping test:
//                connection failure, no fallback (LobbyCutCall). One GGPO battle per process.
//   delay=0      GGPO frame delay of the local input (fixed). Without it a lobby battle picks
//                max(mindelay, ceil(slowest peer's rtt / 2 / 16 ms)) when GGPO arms; rtt from a ping
//                test on the GGPO port (flycast UdpPingPong packets, Zdxsv::StartPingTest)
//   mindelay=2   lower bound of that pick
//   badsession=1 test: this client's ping test uses another session id, so no peer answers it (the cut)
//   advertise=P  lobby test: the platform info announces 127.0.0.1 and GGPO port P only (no STUN /
//                local / IPv6 address), so peers reach us through a localhost udprelay.py at P
//   replay=DIR   net: save the battle to DIR/<battle_code>.zdxr (frame 0 state + all inputs, ReplayWrite);
//                lobby=1 saves to <data dir>/replays by default; replay=0 = off
//   osd=1        net: network status OSD (GgpoOsdLines; 0 = off); its text is also logged every 600 frames
//   sync=0       no state hashes: checksum 0 (net: always)
//   start=1500   vsync (counted from boot) the session starts at
//   frames=3000  frames the session runs, then it is closed and reported
//   check=6      synctest check distance (1..6)
//   hash=pw      synctest checksum: the player work of all 4 players, masked as the H lines (PwHash),
//                and the game RNG words (without them a rerun with other inputs went unseen, s756);
//                pos = the 4 players' x, y, z + game RNG (PosRng); full = EE RAM + delta state (code-cache
//                noise: the rerun's IOP/event cycles differ, so it always reports mismatches)
//   seed=1       random pad input (both pads; a new input every 5 frames)
//   input=host   pad 1 from the host pad instead of random (pad 2 stays random)
//   input=none   no buttons, sticks centered
//   mask=fcff    random buttons limited to these bits (hex, PadDualshock2::Inputs; fcff = no Select/Start)
//   control=input  control run: reruns get other inputs (must report mismatches)
// Results go to the log, lines start with "ZdxsvGgpo".

#include "Zdxsv/Ggpo.h"
#include "Zdxsv/DeltaState.h"
#include "Zdxsv/Lobby.h"
#include "Zdxsv/TestOptions.h"
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
#include "GS/GSPerfMon.h"
#include "Zdxsv/MediaHooks.h"

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Console.h"
#include "common/Error.h"
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
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cctype>
#include <deque>
#include <optional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#include <limits>

namespace Zdxsv
{
	// ZDXSV_REPLAY=file.zdxr: plays a saved replay (PlayLoad), the net=1 hooks on, no GGPO session
	const char* const s_play_env = std::getenv("ZDXSV_REPLAY");
	bool g_ggpo_enabled = false; // GgpoOnVmInitialize
	bool g_mtvu_off = false; // GgpoOnVmInitialize, cleared at VM shutdown
	namespace
	{
		// The ZDXSV_GGPO options of this VM: the variable, else DEFAULT_OPTIONS for the Z game with the
		// ZdxsvGgpo setting on, else empty = off. Set by GgpoOnVmInitialize before the CPU runs.
		std::string s_options;
		constexpr const char* GAME_SERIAL = "SLPS-25419";
		constexpr u32 GAME_CRC = 0x435D8236; // ELF CRC of SLPS_254.19: the hooks' fixed guest addresses are this build's
		constexpr const char* DEFAULT_OPTIONS = "net=1,lobby=1";
	} // namespace
	bool g_z_game = false;
	bool g_ggpo_active = false;
	bool g_ggpo_in_rollback = false;
	bool g_gs_rerun_frame = false;

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
		// Input of frame f = (A, B) of the host pad (ZdPadAB) + the kind-3 msgs the game sent at frame f - K3_LAG.
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
		bool s_net_env = false; // net=1 in s_options, or a replay plays (GgpoOnVmInitialize)
		// ZDXSV_RBK=i/N (net=1; flycast rbk_test): started from a post-entry state (zdxsv/rbkprep.sh)
		// as battle position i (0-based) of N. Until GGPO arms, every lobby / battle connect RPC is
		// answered here (RbkCall: built-in battle start, recorded connect results, own battle msgs
		// echoed per remote position) and the limiter runs turbo; the process exits at the session end.
		// ZDXSV_RBK_TIME=s: rule time limit (recorded 210). ZDXSV_RBK_COUNT=n: battles (recorded 0 =
		// rematch by input). ZDXSV_RBK_GAUGE=v: 戦力ゲージ (recorded 600). ZDXSV_RAND_INPUT=seed: pad input.
		int s_rbk_me = -1, s_rbk_n = 0;
		const bool s_rbk = [] {
			const char* e = Zdxsv::TestEnv("ZDXSV_RBK");
			return e && std::sscanf(e, "%d/%d", &s_rbk_me, &s_rbk_n) == 2 && s_rbk_me >= 0 && s_rbk_me < s_rbk_n && s_rbk_n <= 4;
		}();
		const char* s_rand_env = Zdxsv::TestEnv("ZDXSV_RAND_INPUT");
		bool s_net = false; // net=1 parsed
		int s_players = 4; // players= (net)
		int s_relay = 0; // relay=R (net): remote p is at port R + 8 * me + p (zdxsv/udprelay.py per pair), not port + p
		u32 s_zds_echo = 0, s_zds_skip = 0;
		// ZDXSV_PW_HASH=1: per GGPO frame (last save wins) XXH3 of each player work 0x8395d8 + 0x2200*p,
		// written as `H frame h0 h1 h2 h3` to NET_TRACE at the report: the 4 machines simulate every
		// player, so in sync these agree across peers (own-position RAM elsewhere does not, sync=0).
		const bool s_pw_hash = [] {
			const char* e = Zdxsv::TestEnv("ZDXSV_PW_HASH");
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
		// XXH3 of player p's work with the fields above masked (H lines, synctest hash=pw).
		u64 PwHash(u32 p)
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
			return XXH3_64bits(w.data(), PW_SIZE);
		}
		// Synctest hash=pos (inada-s/ai-automation#62): x, y, z (3 floats at player work + 0x2a8) of the 4
		// players, then u16 0x6d7940 and u16 0x6d793c (the game RNGs, generators 0x20f4b0 / 0x20f4f0).
		constexpr u32 PW_POS = 0x2a8, RNG_A = 0x6d7940, RNG_B = 0x6d793c;
		std::array<u8, 4 * 12 + 4> PosRng()
		{
			std::array<u8, 4 * 12 + 4> b;
			for (u32 p = 0; p < 4; p++)
				std::memcpy(&b[12 * p], &eeMem->Main[PW_BASE + PW_SIZE * p + PW_POS], 12);
			std::memcpy(&b[48], &eeMem->Main[RNG_A], 2);
			std::memcpy(&b[50], &eeMem->Main[RNG_B], 2);
			return b;
		}
		// ZDXSV_PW_DUMP=file: every save appends (s32 frame, 4 * PW_SIZE bytes of player work); rollback
		// re-saves a frame, the last record wins (`zdxsv/pwdiff.py` finds the fields behind H mismatches).
		std::FILE* s_pw_dump = [] {
			const char* p = Zdxsv::TestEnv("ZDXSV_PW_DUMP");
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
		bool s_zds_ps = Zdxsv::TestEnv("ZDXSV_ZDS_PS") != nullptr; // replay: the file's zds_ps
		struct PS
		{
			u8 n, rel;
			bool hold, go;
		} s_ps = {};
		Input s_zd_pad[128] = {}; // own host pad per frame & 127 (reruns reapply it)
		u16 s_zd_hist[128][GGPO_MAX_PLAYERS][2] = {}; // synced (A, B) per frame & 127
		u32 s_zd_steps = 0, s_zd_changed = 0;
		bool s_net_armed = false, s_net_over = false;
		bool s_lobby_cut = false; // lobby=1 ping test failed: the battle connection is silent (LobbyCutCall)
		u32 s_cut_sends = 0;
		int s_net_me = -1; // local battle position
		std::vector<std::vector<u8>> s_net_sent; // every msg the game sent since armed, in order
		size_t s_net_pos = 0; // msgs sent so far in the current timeline
		// ZDXSV_K3_LAG (default 8): a msg sent at GGPO frame s goes into the local input of frame s + lag. A send
		// first seen in a rollback rerun (s645 r1: K3 #4 released in a rerun, the reply sent there) used to go
		// into the next forward frame's input, so the handshake frame depended on input arrival timing. With
		// lag > GGPO's 6 prediction frames, frame s is final when s + lag is added. 0 = the old behaviour.
		const int s_k3_lag = [] {
			const char* e = Zdxsv::TestEnv("ZDXSV_K3_LAG");
			return e ? std::max(0, std::atoi(e)) : 8;
		}();
		struct NetOut
		{
			int frame; // GGPO frame of the send
			size_t idx; // index in s_net_sent
			std::vector<u8> m;
		};
		std::deque<NetOut> s_net_out; // committed, not yet in a local input
		std::vector<int> s_net_sent_at; // GGPO frame of each s_net_sent entry (latest timeline)
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
			const char* e = Zdxsv::TestEnv("ZDXSV_NET_TAIL");
			return e ? std::atoi(e) : 300;
		}();
		struct NetStats
		{
			u32 sends, msgs, recvs, rxmsgs, rxbytes, polls, other, nowait, senddiff, toolong, maxq, restamp, late;
		} s_ns = {};
		bool NetStart(GGPOSessionCallbacks& cb);
		bool NetNextInputs();
		bool NetSyncAndApply();
		void NetApply(const NetInput* in);
		void NetSaved(int frame);
		void NetLoaded(int frame);
		void NetReport();
		void PlayLoad();
		void PlayNext();

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
		bool s_sync = true; // sync=0: no state hashes (checksum 0)
		enum class Hash
		{
			Pw,
			Pos,
			Full
		} s_hash = Hash::Pw; // hash= (synctest checksum)
		int s_port = 7001, s_delay = 0;
		bool s_delay_set = false; // delay= given: fixed; else a lobby battle picks it from the peers' rtt
		int s_min_delay = 2; // mindelay=
		std::string s_peer_host = "127.0.0.1";
		bool s_lobby = false; // lobby=1
		bool s_bad_session = false; // badsession=1 (test): the ping test uses another session id
		bool s_osd = true; // osd=
		std::string s_replay_dir; // replay=DIR; without it lobby=1 saves to <data dir>/replays, other net runs none
		bool s_replay_off = false; // replay=0
		// network status OSD: user id + name per position (lobby battle info), lines of the last frame
		std::vector<std::pair<std::string, std::string>> s_lobby_players, s_net_players;
		std::mutex s_osd_mtx;
		std::vector<GgpoOsdLine> s_osd_lines;
		// lobby=1: peers of the last battle info (SetLobbyPeers, DEV9 thread)
		std::mutex s_lobby_mtx;
		bool s_lobby_info = false, s_lobby_ok = false, s_lobby_logged = false, s_lobby_unreachable = false;
		std::vector<std::vector<Zdxsv::PeerAddr>> s_lobby_peers; // candidates per position
		std::vector<Zdxsv::PeerAddr> s_net_peers; // per position, picked when armed
		std::vector<int> s_net_via; // per position: PingResult.via (0 direct, 1 peer relay, 2 relay server)
		std::vector<Zdxsv::RelayServerAddr> s_net_servers; // relay servers, registered with GGPO in order
		std::vector<Zdxsv::BattleInfo::Relay> s_lobby_relays; // relay servers of the last battle info
		u32 s_lobby_session = 0; // ggpo_session
		u32 s_lobby_gen = 0, s_armed_gen = 0; // battle infos received; the one the last GGPO battle armed with
		std::string s_report_ids; // battle info lines naming the battle (SetLobbyPeers)
		std::string s_report; // P2PMatchingReport of the last lobby battle (TakeLobbyReport)
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
		bool s_started = false; // the session was opened (lobby=1: until the next battle's NetReset)
		bool s_frame_ended = false; // the CPU left Execute() at a vsync
		bool s_vm_closing = false; // in GgpoOnVmShutdown: Stop does not shut the VM down (rbk)
		int s_vsyncs = 0;
		int s_session_frames = 0;
		std::mt19937 s_rng;
		Input s_random[PLAYERS] = {};
		std::array<float, PadDualshock2::Inputs::LENGTH> s_host = {};

		int s_rollback_frames = 0, s_loads = 0, s_mismatches = 0, s_ggpo_warnings = 0;
		Stat s_save_ms, s_hash_ms, s_load_ms;
		Stat s_rerun_ms, s_wait_ms; // between frames: rollback rerun emulation, net wait for a peer
		Stat s_emu_ms, s_exit_ms, s_ours_ms; // wall: last GgpoOnExecuteReturned end -> GgpoOnVsync -> GgpoOnExecuteReturned start -> its end
		Common::Timer::Value s_t_vsync = 0, s_t_returned = 0;
		// Output of the session: GS frames presented (g_perfmon, GS thread; read here for the report only)
		// since the start, SPU2 samples played and dropped in rerun frames.
		int s_gs_frame0 = 0;
		s64 s_spu_played = 0, s_spu_dropped = 0;

		void Parse()
		{
			for (const std::string_view item : StringUtil::SplitString(s_options, ','))
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
				else if (key == "hash")
					s_hash = value == "full" ? Hash::Full : value == "pos" ? Hash::Pos : Hash::Pw;
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
				else if (key == "port")
					s_port = n;
				else if (key == "host")
					s_peer_host = std::string(value);
				else if (key == "delay")
				{
					s_delay = n;
					s_delay_set = true;
				}
				else if (key == "mindelay")
					s_min_delay = n;
				else if (key == "sync")
					s_sync = (n != 0);
				else if (key == "net")
					s_net = (n != 0);
				else if (key == "players")
					s_players = std::clamp(n, 2, GGPO_MAX_PLAYERS);
				else if (key == "relay")
					s_relay = n;
				else if (key == "lobby")
					s_lobby = (n != 0);
				else if (key == "badsession")
					s_bad_session = (n != 0);
				else if (key == "osd")
					s_osd = (n != 0);
				else if (key == "advertise")
					; // GgpoLobbyAdvertisePort
				else if (key == "replay")
				{
					s_replay_off = (value == "0");
					s_replay_dir = s_replay_off ? std::string() : Path::ToNativePath(value); // '/' fails on Windows
				}
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
			Console.WriteLn("ZdxsvGgpo: %s frames %d rollback frames %d loads %d mismatches %d ggpo warnings %d | save ms mean %.3f max %.3f skipped %d | hash ms mean %.3f | load ms mean %.3f max %.3f",
				what, s_session_frames, s_rollback_frames, s_loads, s_mismatches, s_ggpo_warnings, s_save_ms.Mean(), s_save_ms.max, s_save_skipped,
				s_hash_ms.Mean(), s_load_ms.Mean(), s_load_ms.max);
			Console.WriteLn("ZdxsvGgpo: %s wall ms per frame: emulate mean %.2f max %.1f | exit %.2f | between frames (save, ggpo, rollbacks) mean %.2f max %.1f",
				what, s_emu_ms.Mean(), s_emu_ms.max, s_exit_ms.Mean(), s_ours_ms.Mean(), s_ours_ms.max);
			const double n = std::max(s_session_frames, 1);
			const double ours = s_ours_ms.sum / n, split = (s_save_ms.sum + s_hash_ms.sum + s_load_ms.sum + s_rerun_ms.sum + s_wait_ms.sum) / n;
			Console.WriteLn("ZdxsvGgpo: %s between frames ms per frame %.2f: save %.2f hash %.2f load %.2f rerun %.2f wait %.2f rest (ggpo) %.2f | sync=%d",
				what, ours, s_save_ms.sum / n, s_hash_ms.sum / n, s_load_ms.sum / n, s_rerun_ms.sum / n, s_wait_ms.sum / n, ours - split, s_sync);
			Console.WriteLn("ZdxsvGgpo: %s delta %s | %s", what, Zdxsv::DeltaStateTimes().c_str(), SaveState_DeltaTimes().c_str());
			Console.WriteLn("ZdxsvGgpo: %s output: presented frames %d | audio samples played %lld dropped (rerun) %lld",
				what, g_perfmon.GetFrame() - s_gs_frame0, static_cast<long long>(s_spu_played), static_cast<long long>(s_spu_dropped));
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

		// hash=pw / pos: the differing u32 words, as player work p + offset (pw) or PosRng byte offset (pos).
		void DiffRaw(int frame, const std::vector<u8>& first, const std::vector<u8>& raw)
		{
			for (size_t w = 0; w + 4 <= std::min(first.size(), raw.size()) && s_diff_logged < 60; w += 4)
			{
				u32 x, y;
				std::memcpy(&x, &first[w], 4);
				std::memcpy(&y, &raw[w], 4);
				if (x == y)
					continue;
				if (s_hash == Hash::Pw && w == 4 * PW_SIZE)
					Console.WriteLn("ZdxsvGgpo: DIFF frame %d rng: %08x -> %08x", frame, x, y);
				else if (s_hash == Hash::Pw)
					Console.WriteLn("ZdxsvGgpo: DIFF frame %d player %zu +0x%04zx: %08x -> %08x", frame, w / PW_SIZE, w % PW_SIZE, x, y);
				else
					Console.WriteLn("ZdxsvGgpo: DIFF frame %d posrng +%zu: %08x -> %08x", frame, w, x, y);
				s_diff_logged++;
			}
		}

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

		// per position: 0 connected, 1 interrupted, 2 disconnected (GGPO events, for the OSD)
		int s_peer_state[GGPO_MAX_PLAYERS] = {};
		void SetPeerState(GGPOPlayerHandle h, int state)
		{
			for (int p = 0; p < GGPO_MAX_PLAYERS; p++)
				if (s_handles[p] == h)
					s_peer_state[p] = state;
		}

		constexpr u32 OsdColor(u32 r, u32 g, u32 b) { return 0xff000000u | (b << 16) | (g << 8) | r; } // IM_COL32
		constexpr u32 OSD_TEXT = OsdColor(255, 255, 255);
		// flycast msColor
		u32 OsdPingColor(int ms)
		{
			return ms <= 0 ? OsdColor(64, 64, 64) : ms <= 30 ? OsdColor(87, 213, 213) : ms <= 60 ? OsdColor(0, 255, 149) :
			       ms <= 90 ? OsdColor(255, 255, 0) : ms <= 120 ? OsdColor(255, 170, 0) : OsdColor(255, 0, 0);
		}

		// net, once per frame: the OSD lines (GgpoOsdLines). log: also to the log, one line.
		void UpdateOsd(bool log)
		{
			if (!s_osd || !s_net || !s_session)
				return;
			std::vector<GgpoOsdLine> lines;
			// flycast's delay colors
			lines.push_back({fmt::format("Delay {}fr", s_delay), s_delay >= 13 ? OsdColor(255, 38, 31) : s_delay >= 10 ? OsdColor(255, 128, 0) :
			                                                      s_delay >= 5   ? OsdColor(255, 217, 0) : OSD_TEXT});
			lines.push_back({fmt::format("Roll {}  Wait {}", s_rollback_frames, s_waits), OSD_TEXT});
			for (int p = 0; p < s_players; p++)
			{
				if (p == s_net_me)
					continue;
				const auto& who = p < static_cast<int>(s_net_players.size()) ? s_net_players[p] : std::pair<std::string, std::string>{};
				lines.push_back({fmt::format("{}P {}", p + 1, who.first), OSD_TEXT});
				if (!who.second.empty())
					lines.push_back({" " + who.second, OSD_TEXT});
				GGPONetworkStats st{};
				if (s_peer_state[p] == 2)
					lines.push_back({" Disconnected", OsdPingColor(999)});
				else if (ggpo_get_network_stats(s_session, s_handles[p], &st) == GGPO_OK)
					lines.push_back({fmt::format(" Ping {}ms{}  P {}{}", st.network.ping,
					                     p < static_cast<int>(s_net_via.size()) && s_net_via[p] ? " (R)" : "", st.sync.predicted_frames,
					                     s_peer_state[p] == 1 ? "  Interrupted" : ""),
						s_peer_state[p] == 1 ? OsdPingColor(999) : OsdPingColor(st.network.ping)});
			}
			if (log)
			{
				std::string all;
				for (const GgpoOsdLine& l : lines)
					all += (all.empty() ? "" : " |") + l.text;
				Console.WriteLn("ZdxsvGgpo: osd frame %d: %s", s_session_frames, all.c_str());
			}
			std::lock_guard lock(s_osd_mtx);
			s_osd_lines = std::move(lines);
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
					SetPeerState(ev->u.disconnected.player, 2);
					Console.Warning("ZdxsvGgpo: peer disconnected");
					break;
				case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
					SetPeerState(ev->u.connection_interrupted.player, 1);
					Console.Warning("ZdxsvGgpo: connection interrupted");
					break;
				case GGPO_EVENTCODE_CONNECTION_RESUMED:
					SetPeerState(ev->u.connection_resumed.player, 0);
					break;
				default:
					break;
			}
			return true;
		}

		void HashSave(int frame, int* checksum);

		bool __cdecl SaveGameState(unsigned char** buffer, int* len, int* checksum, int frame)
		{
			Common::Timer timer;
			int confirmed = -1;
			if (!s_save_all && !s_sync && ggpo_get_last_confirmed_frame(s_session, &confirmed) == GGPO_OK && frame <= confirmed)
				s_save_skipped++;
			else if (!Zdxsv::DeltaStateSave(frame))
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
			Zdxsv::DeltaStateDiscardBefore(frame - std::max(s_check, 8) - 4);
			return true;
		}

		// sync=1 part of SaveGameState: checksum, synctest diff samples.
		void HashSave(int frame, int* checksum)
		{
			if (s_hash != Hash::Full)
			{
				// hash=pw / pos: the sample keeps the raw bytes (4 player works / PosRng) to name what differed.
				std::vector<u8> raw;
				u64 hash;
				if (s_hash == Hash::Pw)
				{
					raw.assign(&eeMem->Main[PW_BASE], &eeMem->Main[PW_BASE + 4 * PW_SIZE]);
					raw.insert(raw.end(), &eeMem->Main[RNG_A], &eeMem->Main[RNG_A + 2]);
					raw.insert(raw.end(), &eeMem->Main[RNG_B], &eeMem->Main[RNG_B + 2]);
					u64 h[5];
					for (u32 p = 0; p < 4; p++)
						h[p] = PwHash(p);
					h[4] = XXH3_64bits(&raw[4 * PW_SIZE], 4);
					hash = XXH3_64bits(h, sizeof(h));
				}
				else
				{
					const auto b = PosRng();
					raw.assign(b.begin(), b.end());
					hash = XXH3_64bits(b.data(), b.size());
				}
				*checksum = static_cast<int>(hash ^ (hash >> 32));
				if (s_rerun)
				{
					const auto first = s_first.find(frame);
					if (first != s_first.end())
						DiffRaw(frame, first->second.state, raw);
				}
				else
				{
					s_first[frame].state = std::move(raw);
					while (!s_first.empty() && s_first.begin()->first < frame - 16)
						s_first.erase(s_first.begin());
				}
				return;
			}
			const std::vector<u8>* state = Zdxsv::DeltaStateGetState(frame);
			Sample sample;
			sample.pages.resize(Ps2MemSize::ExposedRam / PAGE_SIZE);
			for (size_t i = 0; i < sample.pages.size(); i++)
				sample.pages[i] = XXH3_64bits(&eeMem->Main[i * PAGE_SIZE], PAGE_SIZE);
			const u64 ram_hash = XXH3_64bits(sample.pages.data(), sample.pages.size() * sizeof(u64));
			const u64 state_hash = Zdxsv::DeltaStateHash(*state);
			const u64 hash = ram_hash ^ state_hash;
			*checksum = static_cast<int>(hash ^ (hash >> 32));
			std::vector<u8> masked = *state;
			Zdxsv::DeltaStateMaskScratch(masked);
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
			const bool ok = Zdxsv::DeltaStateLoad(*reinterpret_cast<int*>(buffer));
			if (s_net)
				NetLoaded(*reinterpret_cast<int*>(buffer));
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
			g_ggpo_in_rollback = true;
			Common::Timer timer;
			const bool ok = RunFrame();
			s_rerun_ms.Add(timer.GetTimeMilliseconds());
			g_ggpo_in_rollback = false;
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
			if (s_ggpo_warnings++ < 20)
				Console.Warning("ZdxsvGgpo: ggpo: %s", msg);
		}

		// replay= (net): the battle as the full state at GGPO frame 0 (.p2s, zipped on a thread while the battle
		// runs) + the synced inputs of every player per frame. File format: see ReplayWrite.
		std::string s_replay_state; // the frame 0 .p2s being written, "" = not recording
		struct ReplayZip
		{
			std::thread t;
			bool ok = false; // set by t before it ends
			~ReplayZip()
			{
				if (t.joinable())
					t.join();
			}
		} s_replay_zip;
		std::vector<NetInput> s_replay_inputs; // [frame * s_players + position]
		s64 s_replay_start_at = 0; // unix seconds
		int s_replay_confirmed = -1; // GGPO's last confirmed frame: the frames after it (predicted inputs) are not written
		std::string s_replay_hle0; // header lines zds_ps, rx0, hle0 (ReplayBegin)

		std::string ReplayDir()
		{
			if (s_replay_off || !s_net)
				return {};
			if (!s_replay_dir.empty())
				return s_replay_dir;
			return s_lobby ? Path::Combine(EmuFolders::DataRoot, "replays") : std::string();
		}

		// At GGPO frame 0: the session started, its first input not added yet (Zdxsv::DeltaStateSave(0) saves this point).
		void ReplayBegin()
		{
			const std::string dir = ReplayDir();
			if (dir.empty())
				return;
			Error error;
			if (!FileSystem::DirectoryExists(dir.c_str()) && !FileSystem::CreateDirectoryPath(dir.c_str(), true, &error))
			{
				Console.Error("ZdxsvGgpo: replay: cannot create %s: %s", dir.c_str(), error.GetDescription().c_str());
				return;
			}
			Common::Timer timer;
			std::unique_ptr<ArchiveEntryList> list = SaveState_DownloadState(&error);
			if (!list)
			{
				Console.Error("ZdxsvGgpo: replay: state download failed: %s", error.GetDescription().c_str());
				return;
			}
			s_replay_start_at = static_cast<s64>(std::time(nullptr));
			s_replay_state = Path::Combine(dir, fmt::format(".replay-{}-p{}.p2s", s_replay_start_at, s_net_me));
			s_replay_inputs.clear();
			s_replay_confirmed = -1;
			// HLE state outside the save state at frame 0 (PlayLoad restores it): msgs waiting for the game's recv,
			// play-start barrier, kind-3 barrier
			std::string rx;
			for (u8 v : s_net_rx)
				rx += fmt::format("{:02x}", v);
			s_replay_hle0 = fmt::format("zds_ps={}\nrx0={}\nhle0={},{},{},{},{},{},{},{},{}\n", s_zds_ps ? 1 : 0, rx, s_ps.n, s_ps.rel,
				s_ps.hold ? 1 : 0, s_ps.go ? 1 : 0, s_zds_seen[0], s_zds_seen[1], s_zds_seen[2], s_zds_seen[3], s_zds_rel);
			s_replay_zip.ok = false;
			s_replay_zip.t = std::thread([list = std::move(list), path = s_replay_state]() mutable {
				Error error;
				s_replay_zip.ok = SaveState_ZipToDisk(std::move(list), nullptr, path.c_str(), &error);
				if (!s_replay_zip.ok)
					Console.Error("ZdxsvGgpo: replay: state zip failed: %s", error.GetDescription().c_str());
			});
			Console.WriteLn("ZdxsvGgpo: replay: recording to %s (state download %.1f ms)", dir.c_str(), timer.GetTimeMilliseconds());
		}

		// The synced inputs of frame f (also rerun frames: a rollback replaces the frames from f on).
		void ReplayLog(int f, const NetInput* in)
		{
			if (s_replay_state.empty() || f < 0)
				return;
			s_replay_inputs.resize(static_cast<size_t>(f) * s_players);
			s_replay_inputs.insert(s_replay_inputs.end(), in, in + s_players);
			int confirmed = -1;
			if (s_session && ggpo_get_last_confirmed_frame(s_session, &confirmed) == GGPO_OK)
				s_replay_confirmed = std::max(s_replay_confirmed, confirmed);
		}

		// <dir>/<battle_code>.zdxr (without a battle code: rbk-<start_at>-p<position>.zdxr):
		//   "ZDXSV-REPLAY 1\n", key=value lines (battle info ids, players, position, delay, start_at, end_at,
		//   frames, close, user_<p>, name_<p>, zds_ps, rx0 (hex), hle0 (ps n,rel,hold,go, k3 seen 0..3,rel), input_size,
		//   state_size), an empty line,
		//   state_size bytes: the .p2s at frame 0, then frames * players NetInput (input_size bytes each,
		//   frame-major, by battle position).
		void ReplayWrite(const char* what)
		{
			if (s_replay_state.empty())
				return;
			Common::Timer timer;
			if (s_replay_zip.t.joinable())
				s_replay_zip.t.join();
			const std::string state_path = std::exchange(s_replay_state, {});
			const std::optional<std::vector<u8>> state = s_replay_zip.ok ? FileSystem::ReadBinaryFile(state_path.c_str()) : std::nullopt;
			FileSystem::DeleteFilePath(state_path.c_str());
			if (!state)
			{
				Console.Error("ZdxsvGgpo: replay: no frame 0 state, nothing saved");
				return;
			}
			std::string ids;
			{
				std::lock_guard lock(s_lobby_mtx);
				ids = s_lobby ? s_report_ids : std::string();
			}
			std::string name;
			if (const size_t at = ids.find("battle_code="); at != std::string::npos)
				for (size_t i = at + 12; i < ids.size() && ids[i] != '\n'; i++)
					name += std::isalnum(static_cast<unsigned char>(ids[i])) || ids[i] == '-' || ids[i] == '_' ? ids[i] : '_';
			if (name.empty())
				name = fmt::format("rbk-{}-p{}", s_replay_start_at, s_net_me);
			const size_t frames = std::min<size_t>(s_replay_inputs.size() / s_players, s_replay_confirmed + 1);
			std::string header = "ZDXSV-REPLAY 1\n" + ids;
			header += fmt::format("players={}\nposition={}\ndelay={}\nstart_at={}\nend_at={}\nframes={}\nclose={}\n", s_players, s_net_me,
				s_delay, s_replay_start_at, static_cast<s64>(std::time(nullptr)), frames, what);
			for (size_t p = 0; p < s_net_players.size(); p++)
				header += fmt::format("user_{}={}\nname_{}={}\n", p, s_net_players[p].first, p, s_net_players[p].second);
			header += s_replay_hle0;
			header += fmt::format("input_size={}\nstate_size={}\n\n", sizeof(NetInput), state->size());
			const std::string path = Path::Combine(Path::GetDirectory(state_path), name + ".zdxr");
			Error error;
			auto fp = FileSystem::OpenManagedCFile(path.c_str(), "wb", &error);
			const size_t input_bytes = frames * s_players * sizeof(NetInput);
			if (!fp || std::fwrite(header.data(), 1, header.size(), fp.get()) != header.size() ||
				std::fwrite(state->data(), 1, state->size(), fp.get()) != state->size() ||
				std::fwrite(s_replay_inputs.data(), 1, input_bytes, fp.get()) != input_bytes || std::fflush(fp.get()) != 0)
			{
				Console.Error("ZdxsvGgpo: replay: write %s failed %s", path.c_str(), error.GetDescription().c_str());
				return;
			}
			Console.WriteLn("ZdxsvGgpo: replay saved %s frames=%zu state=%zu bytes, %.1f ms", path.c_str(), frames, state->size(),
				timer.GetTimeMilliseconds());
		}

		void Stop(const char* what)
		{
			if (s_session)
				ggpo_close_session(s_session);
			s_session = nullptr;
			ggpo_set_log_function(nullptr);
			g_ggpo_active = false;
			Zdxsv::DeltaStateClear();
			Report(what);
			{
				std::lock_guard lock(s_osd_mtx);
				s_osd_lines.clear();
			}
			if (s_lobby)
			{
				std::lock_guard lock(s_lobby_mtx);
				if (!s_report.empty())
					s_report += std::string("close=") + what + "\nframes=" + std::to_string(s_session_frames) +
					            "\nrollback_frames=" + std::to_string(s_rollback_frames) + "\nmismatches=" + std::to_string(s_mismatches) +
					            "\ndisconnected=" + (s_disconnected ? "1" : "0") + "\n";
			}
			if (s_net)
			{
				s_net_over = true; // the battle sock goes back to the IOP
				ReplayWrite(what);
				NetReport();
			}
			if (s_rbk && !s_vm_closing)
			{
				Console.WriteLn("ZdxsvGgpo: rbk exit (%s) at vsync %u", what, g_FrameCount);
				Host::RunOnCPUThread([] { Host::RequestVMShutdown(false, false, false); });
			}
		}

		// lobby=1: the next lobby battle in this process starts from the state the first one started from
		// (Stop closed the session; its stats are reported).
		void NetReset()
		{
			s_started = s_net_armed = s_net_over = s_lobby_cut = false;
			s_frame_ended = s_running = s_disconnected = s_rerun = false;
			s_net_me = -1;
			s_cut_sends = 0;
			s_net_sent.clear();
			s_net_sent_at.clear();
			s_net_pos = 0;
			s_net_out.clear();
			s_net_local = {};
			std::memset(s_net_seq_at, 0, sizeof(s_net_seq_at));
			s_net_rx.clear();
			s_net_frame = 0;
			std::fill(std::begin(s_net_at), std::end(s_net_at), NetFrame{});
			s_net_end = -1;
			s_ns = {};
			s_ps = {};
			std::fill(std::begin(s_zd_pad), std::end(s_zd_pad), Input{});
			std::memset(s_zd_hist, 0, sizeof(s_zd_hist));
			s_zd_steps = s_zd_changed = s_zds_echo = s_zds_skip = s_zds_k3rel = 0;
			for (auto& k3 : s_zds_k3)
				k3.clear();
			std::fill(std::begin(s_zds_seen), std::end(s_zds_seen), 0);
			s_zds_rel = 0;
			s_pw.clear();
			std::fill(std::begin(s_peer_state), std::end(s_peer_state), 0);
			std::fill(std::begin(s_handles), std::end(s_handles), GGPOPlayerHandle{});
			s_frames_ahead = s_waits = s_save_skipped = s_session_frames = 0;
			s_rollback_frames = s_loads = s_mismatches = s_ggpo_warnings = s_diff_logged = 0;
			s_save_ms = s_hash_ms = s_load_ms = s_rerun_ms = s_wait_ms = s_emu_ms = s_exit_ms = s_ours_ms = {};
			s_t_vsync = s_t_returned = 0;
			s_spu_played = s_spu_dropped = 0;
		}

		// lobby=1: a battle info came after the one the last GGPO battle armed with
		bool LobbyNewBattle()
		{
			std::lock_guard lock(s_lobby_mtx);
			return s_lobby_gen != s_armed_gen;
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
			Console.WriteLn("ZdxsvGgpo: synctest start=%d frames=%d check=%d seed=%u input=%s mask=%04x control=%d hash=%s",
				s_start, s_frames, s_check, s_seed, s_host_input ? "host" : s_no_input ? "none" : "random", s_mask, s_control_input,
				s_hash == Hash::Full ? "full" : s_hash == Hash::Pos ? "pos" : "pw");
			return true;
		}
	} // namespace

	void TracePad();
	void TraceInputs();

	void GgpoOnVsync()
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
		// ZDXSV_VM_TEST=frame:shutdown|reset (tests of GgpoOnVmShutdown): once, when a session or replay reaches GGPO frame `frame`
		static const char* vm_test = std::getenv("ZDXSV_VM_TEST");
		if (vm_test && g_ggpo_active && !g_ggpo_in_rollback && s_net_frame == std::atoi(vm_test))
		{
			vm_test = nullptr;
			if (std::strstr(std::getenv("ZDXSV_VM_TEST"), ":reset"))
				Host::RunOnCPUThread([] { VMManager::Reset(); });
			else
				Host::RunOnCPUThread([] { Host::RequestVMShutdown(false, false, false); });
			Console.WriteLn("ZdxsvGgpo: vm test %s at frame %d", std::getenv("ZDXSV_VM_TEST"), s_net_frame);
		}
		TracePad();
		TraceInputs();
		// ZDXSV_SNAP=dir,n: GS screenshot dir/v<vsync>.png every n vsyncs (needs a real renderer, not -Headless)
		static const char* snap = Zdxsv::TestEnv("ZDXSV_SNAP");
		static const int snap_n = snap && std::strchr(snap, ',') ? std::atoi(std::strchr(snap, ',') + 1) : 0;
		if (snap_n > 0 && !g_ggpo_in_rollback && g_FrameCount % snap_n == 0)
			GSQueueSnapshot(fmt::format("{}\\v{}.png", std::string(snap, std::strchr(snap, ',')), g_FrameCount));
		if (!g_ggpo_enabled)
			return;
		if (!g_ggpo_active)
		{
			if (!g_ggpo_enabled || s_started)
				return;
			if (s_vsyncs++ == 0)
			{
				Parse();
				if (s_play_env)
					Host::RunOnCPUThread(PlayLoad);
			}
			if (s_play_env) // PlayLoad starts it
				return;
			if (s_net ? !s_net_armed : s_vsyncs < s_start)
				return;
			s_started = true;
			g_ggpo_active = true;
			s_gs_frame0 = g_perfmon.GetFrame();
		}
		s_frame_ended = true;
		if (!g_ggpo_in_rollback)
		{
			s_t_vsync = Common::Timer::GetCurrentValue();
			if (s_t_returned)
				s_emu_ms.Add(Common::Timer::ConvertValueToMilliseconds(s_t_vsync - s_t_returned));
		}
		Cpu->ExitExecution();
	}

	static void Returned();

	bool SpuOnOutput()
	{
		(g_ggpo_in_rollback ? s_spu_dropped : s_spu_played)++;
		return g_ggpo_in_rollback;
	}

	void GgpoOnExecuteReturned()
	{
		if (!g_ggpo_active || !std::exchange(s_frame_ended, false))
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
		if (s_play_env)
		{
			PlayNext();
			return;
		}
		if (!s_session)
		{
			if (!Start())
			{
				Stop("start failed");
				return;
			}
			if (s_net)
				ReplayBegin();
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
		UpdateOsd(s_session_frames % 600 == 0);
		if (!NextInputs())
			Stop("failed");
	}

	bool GgpoCaptureHostInput(u32 controller, u32 bind, float value)
	{
		if (!g_ggpo_active)
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
			const char* p = Zdxsv::TestEnv("ZDXSV_NET_TRACE");
			std::FILE* f = p ? std::fopen(p, "w") : nullptr;
			if (f) // unbuffered: the rig kills pcsx2, buffered lines would be lost (zdxsv/probelint.py)
				std::setvbuf(f, nullptr, _IONBF, 0);
			return f;
		}();
	} // namespace

	bool g_net_hook = false; // GgpoOnVmInitialize
	bool g_zd_hook = false;
	bool g_ps_hook = s_zds_ps;

	// Release build: of ZDXSV_GGPO only the keys that change nothing for the other players (osd=, port=, replay=0)
	// are kept, as ",k=v,..." for DEFAULT_OPTIONS (net=1 and lobby=1 are in it already). delay=/mindelay= are
	// test options too: the ping test picks the delay.
	static std::string PlayerOptions(const char* e)
	{
		std::string kept;
		for (const std::string_view item : StringUtil::SplitString(e ? e : "", ','))
		{
			if (item == "net=1" || item == "lobby=1")
				continue;
			if (item.starts_with("osd=") || item.starts_with("port=") || item == "replay=0")
				kept += fmt::format(",{}", item);
			else
				Console.Warning("ZdxsvGgpo: '%.*s' ignored: a test option, needs a ZDXSV_TEST_OPTIONS build",
					static_cast<int>(item.size()), item.data());
		}
		return kept;
	}

	void GgpoOnVmInitialize(const char* serial, u32 crc)
	{
		const char* e = std::getenv("ZDXSV_GGPO");
		// Read here only, so not a config field: a change takes effect at the next VM start.
		const bool setting = Host::GetBoolSettingValue("DEV9/Eth", "ZdxsvGgpo", true);
		// ZDXSV_GAME_CRC=hex (test): the CRC taken as the Z game's, to run the wrong-game path on the Z game
		const char* want_env = Zdxsv::TestEnv("ZDXSV_GAME_CRC");
		const u32 want = want_env ? static_cast<u32>(std::strtoul(want_env, nullptr, 16)) : GAME_CRC;
		const bool serial_match = std::strcmp(serial, GAME_SERIAL) == 0;
		g_z_game = serial_match && crc == want;
		const bool lobby_default = g_z_game && setting && !s_play_env;
		if (!g_z_game) // any other game or build: no GGPO, replay, hooks or platform info, in test builds too
		{
			s_options.clear();
			g_ggpo_enabled = s_net_env = g_net_hook = g_zd_hook = g_ps_hook = false;
			if (serial_match || (e && std::strcmp(e, "0") != 0) || s_play_env || s_net_trace)
				Console.Warning("ZdxsvGgpo: off: not the Z game (serial %s CRC %08X, need %s %08X)", serial, crc, GAME_SERIAL, want);
			return;
		}
		if (e && std::strcmp(e, "0") == 0) // off whatever the setting (rigs without GGPO)
			s_options.clear();
		else if (TEST_OPTIONS)
			s_options = e ? e : lobby_default ? DEFAULT_OPTIONS : "";
		else // release build: the lobby session only, plus the player's own choices
			s_options = lobby_default ? DEFAULT_OPTIONS + PlayerOptions(e) : "";
		g_ggpo_enabled = !s_options.empty() || s_play_env;
		s_net_env = s_options.find("net=1") != std::string::npos || s_play_env;
		g_net_hook = s_net_trace != nullptr || s_net_env;
		g_zd_hook = s_net_env;
		g_ps_hook = s_zds_ps;
		if (!s_options.empty() || std::strcmp(serial, GAME_SERIAL) == 0)
			Console.WriteLn("ZdxsvGgpo: options '%s' (serial %s, setting %d)", s_options.c_str(), serial, setting ? 1 : 0);
		// Delta saves and loads, replay keys: VU1 memory is copied while the MTVU thread may still run on it.
		// The settings were loaded before this; VMManager::LoadCoreSettings keeps it off on later reloads.
		g_mtvu_off = g_ggpo_enabled || g_delta_state_test_enabled;
		if (g_mtvu_off && EmuConfig.Speedhacks.vuThread)
		{
			EmuConfig.Speedhacks.vuThread = false;
			Console.WriteLn("ZdxsvGgpo: MTVU speedhack off for this VM");
		}
	}

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
		if (!g_ggpo_active || !s_net_armed || s_net_over)
			return false;
		if (!s_ps.hold)
		{
			s_ps.hold = true;
			s_ps.n++;
			if (s_net_trace)
				std::fprintf(s_net_trace, "%u PH%s %d %d\n", g_FrameCount, g_ggpo_in_rollback ? "r" : "", s_net_frame, s_ps.n);
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
		if (!g_ggpo_active || !s_net_armed)
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
			std::fprintf(s_net_trace, "%u Z%s %d %d %02x %04x %04x %04x %04x %02x %d\n", g_FrameCount, g_ggpo_in_rollback ? "r" : "",
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
		if (s_net_trace && !g_ggpo_in_rollback)
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
		if (!s_net_trace || g_ggpo_in_rollback || std::memcmp(cur, last, 9) == 0)
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
			if (const char* e = Zdxsv::TestEnv("ZDXSV_RAM_DUMP"))
			{
				char dir[512];
				u32 a, b, c;
				if (std::sscanf(e, "%511[^,],%u,%u,%u", dir, &a, &b, &c) == 4)
					d = {dir, a, b ? b : 1, c};
			}
			return d;
		}();
		const auto& [ddir, dstart, dstep, dcount] = dump;
		if (!g_ggpo_in_rollback && dcount && g_FrameCount >= dstart && g_FrameCount < dstart + dstep * dcount &&
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
			for (const char* e = Zdxsv::TestEnv("ZDXSV_EE_CLAMP"); e && *e;)
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
				if (!g_ggpo_in_rollback && logged++ < 6)
					Console.WriteLn("ZdxsvGgpo: EE clamp %x %u -> %u at vsync %u", caddr, v, cmax, g_FrameCount);
				v = static_cast<u16>(cmax);
			}
		}
		if (!s_net_trace || g_ggpo_in_rollback || !s_game_gp)
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
				if (!g_ggpo_in_rollback)
					s_zds_echo++;
			}
			else if (!g_ggpo_in_rollback && s_zds_skip++ < 20)
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
				if (s_net_sent_at[s_net_pos] != s_net_frame && m.size() >= 2 && (m[1] >> 4) == 3)
				{
					auto it = std::find_if(s_net_out.begin(), s_net_out.end(), [](const NetOut& o) { return o.idx == s_net_pos; });
					if (it != s_net_out.end())
						it->frame = s_net_frame, s_ns.restamp++;
					else if (s_ns.late++ < 20)
						Console.Warning("ZdxsvGgpo: net rerun send %zu moved %d -> %d after it went into an input", s_net_pos, s_net_sent_at[s_net_pos], s_net_frame);
					if (s_net_trace)
						std::fprintf(s_net_trace, "%u OM %zu %d %d\n", g_FrameCount, s_net_pos, s_net_sent_at[s_net_pos], s_net_frame);
				}
				s_net_sent_at[s_net_pos] = s_net_frame;
			}
			else
			{
				if (m.size() >= 2 && (m[1] >> 4) == 0xf && s_net_end < 0)
					s_net_end = 0;
				s_net_sent.push_back(m);
				s_net_sent_at.push_back(s_net_frame);
				if (m.size() >= 2 && (m[1] >> 4) == 3)
					s_net_out.push_back({s_net_frame, s_net_pos, std::move(m)});
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
					h[p] = PwHash(p);
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
			const int local_port = s_lobby ? s_port : s_port + s_net_me;
			if (ggpo_start_session(&s_session, &cb, "zdxsv", s_players, sizeof(NetInput), static_cast<unsigned short>(local_port), nullptr, 0) != GGPO_OK)
			{
				s_session = nullptr;
				return false;
			}
			// ZDXSV_NET_DISCONNECT_MS=ms (default 5000): longer lets a peer stall (ZDXSV_RAM_DUMP) without a disconnect.
			const char* dms = Zdxsv::TestEnv("ZDXSV_NET_DISCONNECT_MS");
			ggpo_set_disconnect_timeout(s_session, dms ? std::atoi(dms) : 5000);
			ggpo_set_disconnect_notify_start(s_session, 1000);
			// relay servers before the players, in the battle info's order on every peer (ggpo_add_relay_server)
			if (s_lobby)
				for (const Zdxsv::RelayServerAddr& r : s_net_servers)
				{
					if (ggpo_add_relay_server(s_session, r.addr.ip.c_str(), r.addr.port, r.alt.ip.empty() ? nullptr : r.alt.ip.c_str()) != GGPO_OK)
						return false;
					Console.WriteLn("ZdxsvGgpo: relay server %s%s%s", r.addr.String().c_str(), r.alt.ip.empty() ? "" : " alt ",
						r.alt.ip.empty() ? "" : r.alt.String().c_str());
				}
			for (int p = 0; p < s_players; p++)
			{
				GGPOPlayer player{};
				player.size = sizeof(GGPOPlayer);
				player.player_num = p + 1;
				player.type = (p == s_net_me) ? GGPO_PLAYERTYPE_LOCAL : GGPO_PLAYERTYPE_REMOTE;
				if (player.type == GGPO_PLAYERTYPE_REMOTE && s_lobby)
				{
					const Zdxsv::PeerAddr& a = s_net_peers[p];
					StringUtil::Strlcpy(player.u.remote.ip_address, a.ip.c_str(), sizeof(player.u.remote.ip_address));
					player.u.remote.port = a.port;
					player.u.remote.relay = p < static_cast<int>(s_net_via.size()) && s_net_via[p] != 0;
					Console.WriteLn("ZdxsvGgpo: lobby peer position %d at %s%s", p, a.String().c_str(), player.u.remote.relay ? " (relay)" : "");
				}
				else if (player.type == GGPO_PLAYERTYPE_REMOTE)
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
				s_net_me + 1, s_players, local_port, s_delay, s_net_sent.size(), wait.GetTimeSeconds());
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
			while (!s_net_out.empty() && (s_k3_lag == 0 || s_net_out.front().frame + s_k3_lag <= s_net_frame))
			{
				const std::vector<u8>& m = s_net_out.front().m;
				if (s_net_trace)
					std::fprintf(s_net_trace, "%u O %d %zu %d\n", g_FrameCount, s_net_frame, s_net_out.front().idx, s_net_out.front().frame);
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
			NetApply(in);
			return true;
		}

		// The synced inputs of frame s_net_frame (live: GGPO, replay: the file) to the game.
		void NetApply(const NetInput* in)
		{
			const int f = s_net_frame;
			ReplayLog(f, in);
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
					std::fprintf(s_net_trace, "%u K3%s %d %d\n", g_FrameCount, g_ggpo_in_rollback ? "r" : "", f, s_zds_rel);
				if (!g_ggpo_in_rollback)
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
						std::fprintf(s_net_trace, "%u PS%s %d %d\n", g_FrameCount, g_ggpo_in_rollback ? "r" : "", f, s_ps.rel);
				}
			}
		}

		void NetReport()
		{
			Console.WriteLn("ZdxsvGgpo: net sends %u msgs %u (sent %zu, unsent %zu) recvs %u rxmsgs %u rxbytes %u polls %u other %u nowait %u senddiff %u toolong %u maxq %u waits %d k3lag %d restamp %u late %u",
				s_ns.sends, s_ns.msgs, s_net_sent.size(), s_net_out.size(), s_ns.recvs, s_ns.rxmsgs, s_ns.rxbytes, s_ns.polls,
				s_ns.other, s_ns.nowait, s_ns.senddiff, s_ns.toolong, s_ns.maxq, s_waits, s_k3_lag, s_ns.restamp, s_ns.late);
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

		// ZDXSV_REPLAY (s_play_env): the .zdxr's frame 0 state is loaded, the HLE state of frame 0 restored
		// (header rx0 / hle0), then each frame gets the file's synced inputs (NetApply, as a live frame without
		// rollback): the battle sock HLE, zd step copy and both barriers run as live. Own pad 0 = the own
		// position's B bits mapped back to buttons (no sticks in the file). ZDXSV_REPLAY_EXIT=1: exit at the
		// end (else pause); ZDXSV_REPLAY_TURBO=1: turbo limiter. With ZDXSV_PW_HASH + ZDXSV_NET_TRACE the H lines
		// compare to the live battle's (zdxsv/pwcheck.py).
		std::vector<NetInput> s_play_inputs;
		int s_play_frames = 0;

		// Seek: a key every ZDXSV_REPLAY_KEY=n frames played (default 600, 0 = none): the full state as .p2s in the
		// cache folder (download here, zip on a thread) + the HLE state outside it. A seek loads the newest key at or
		// before the target (none when running on from the current frame is closer) and runs up to the target
		// unlimited. ZDXSV_REPLAY_SEEK=at:to[,at:to...]: seek to `to` when frame `at` is reached (tests).
		// ZDXSV_REPLAY_KEY_NOHLE=1: control, keys restore no HLE state.
		struct PlayKey
		{
			std::string path;
			std::thread zip;
			std::atomic<bool> ok{false};
			size_t pos = 0;
			std::vector<u8> rx;
			int k3seen[GGPO_MAX_PLAYERS] = {}, k3rel = 0;
			PS ps = {};
			Input zd_pad[128] = {};
			u16 zd_hist[128][GGPO_MAX_PLAYERS][2] = {};
			u8 seq_at[64][GGPO_MAX_PLAYERS] = {};
			~PlayKey()
			{
				if (zip.joinable())
					zip.join();
			}
		};
		std::map<int, std::unique_ptr<PlayKey>> s_play_keys[GGPO_MAX_PLAYERS]; // by point of view (position)
		const int s_play_key_every = [] {
			const char* e = std::getenv("ZDXSV_REPLAY_KEY");
			return e ? std::atoi(e) : 600;
		}();
		const bool s_play_key_nohle = std::getenv("ZDXSV_REPLAY_KEY_NOHLE") != nullptr;
		std::deque<std::pair<int, int>> s_play_seeks; // ZDXSV_REPLAY_SEEK
		std::atomic<int> s_play_req{INT_MIN}; // requested seek target, INT_MIN = none
		bool s_play_at_end = false; // paused after the last frame (no ZDXSV_REPLAY_EXIT)
		int s_play_target = -1; // seeking: frames run unlimited up to this one
		LimiterModeType s_play_limiter = LimiterModeType::Nominal;
		// skip MS selection (ZDXSV_REPLAY_SKIP_MS, default on as flycast's gdxsv:ReplaySkipMsSelection, 0 = off): from
		// frame 0 the replay runs unlimited to the briefing = the frame tick state 0xc627b4 leaves 8 (battle load) the
		// 2nd time (load 1 below; round 2 loads again with no MS select). Playing from frame 0 again jumps to the
		// briefing.
		const bool s_play_skip_ms = [] {
			const char* e = std::getenv("ZDXSV_REPLAY_SKIP_MS");
			return !e || e[0] != '0';
		}();
		// Battle loads: the frames where the tick state leaves 8, in order. Load 0 ends at MS select, load 1 at the
		// briefing, load 1 + N at the start of round N (a 2-round 1v1: 216, 4184, 4945, 16103, the same
		// frames for both positions). Frames are played in order up to s_play_hi (a forward seek runs every frame between), so
		// the list is complete up to it. Round jump (ZDXSV_REPLAY_ROUND_AT, hotkeys, control bar): a known round
		// start is a seek; an unknown one runs unlimited from s_play_hi until that load ends.
		static constexpr u32 TICK_STATE = 0xc627b4; // u8 game phase; 8 = battle load
		static constexpr u8 TICK_LOAD = 8;
		std::mutex s_battle_loads_mtx;
		std::vector<int> s_battle_loads; // written on the CPU thread; the GS thread reads it under s_battle_loads_mtx
		int s_play_hi = -1, s_tick_f = -1;
		u8 s_tick_st = 0;
		int s_run_load = -1; // running unlimited until this load has ended, -1 = none (skip MS selection = 1)
		std::atomic<int> s_play_round_req{INT_MIN}; // requested round, 0 = briefing, INT_MIN = none
		std::deque<std::pair<int, int>> s_play_round_at; // ZDXSV_REPLAY_ROUND_AT=frame:round,...
		bool s_play_pov_ok[GGPO_MAX_PLAYERS] = {};
		// control bar (ReplayBarInfo): written per played frame on the CPU thread, read by the GS thread's ImGui
		std::atomic<int> s_bar_frame{-1}; // -1 = no replay playing
		std::atomic<int> s_bar_frames{0}, s_bar_pov{-1}, s_bar_target{-1};
		std::atomic<u32> s_bar_povs{0}; // bit p = position p has a file

		void PlayBarPublish()
		{
			u32 povs = 0;
			for (int p = 0; p < s_players; p++)
				povs |= s_play_pov_ok[p] ? (1u << p) : 0u;
			s_bar_frames = s_play_frames;
			s_bar_pov = s_net_me;
			s_bar_povs = povs;
			s_bar_target = s_play_target;
			s_bar_frame = s_net_frame;
		}

		// key display (ReplayKeys): runs of the shown position's B word (ZdPadAB) up to the played frame, newest
		// first, with their frame counts. Per frame one more frame; after a seek or a switch, rebuilt from the file.
		static constexpr size_t KEY_RUNS = 14;
		std::atomic<bool> s_keys_on{[] {
			const char* e = std::getenv("ZDXSV_REPLAY_KEY_DISPLAY");
			return e && e[0] == '1';
		}()};
		std::mutex s_keys_mtx;
		std::deque<std::pair<u16, int>> s_keys; // guarded by s_keys_mtx (the GS thread draws it)
		int s_keys_f = -1, s_keys_me = -1; // frame and position s_keys ends at

		u16 PlayB(int f, int p)
		{
			u16 ab[2];
			std::memcpy(ab, &s_play_inputs[static_cast<size_t>(f) * s_players + p].pad, sizeof(ab));
			return ab[1] & ~1u; // bit 0 = game state, not a key
		}

		void PlayKeysPublish(int f)
		{
			if (!s_keys_on || f < 0 || f >= s_play_frames)
				return;
			std::lock_guard lock(s_keys_mtx);
			if (f == s_keys_f + 1 && s_net_me == s_keys_me)
			{
				const u16 b = PlayB(f, s_net_me);
				if (!s_keys.empty() && s_keys.front().first == b)
					s_keys.front().second++;
				else
				{
					s_keys.emplace_front(b, 1);
					if (s_keys.size() > KEY_RUNS)
						s_keys.pop_back();
				}
			}
			else if (f != s_keys_f || s_net_me != s_keys_me)
			{
				s_keys.clear();
				for (int g = f; g >= 0; g--)
				{
					const u16 b = PlayB(g, s_net_me);
					if (!s_keys.empty() && s_keys.back().first == b)
						s_keys.back().second++;
					else if (s_keys.size() == KEY_RUNS)
						break;
					else
						s_keys.emplace_back(b, 1);
				}
			}
			s_keys_f = f;
			s_keys_me = s_net_me;
			if (f % 600 == 0) // test drivers (zdxsv keycheck.py) compare these with the file
			{
				std::string s;
				for (const auto& [b, n] : s_keys)
					s += fmt::format(" {:04x}*{}", b, n);
				Console.WriteLn("ZdxsvGgpo: replay keys frame %d pos %d:%s", f, s_net_me, s.c_str());
			}
		}

		// At the start of frame f, before its inputs (the point of the frame 0 state).
		void PlayKeySave(int f)
		{
			if (s_play_key_every <= 0 || f % s_play_key_every != 0 || s_play_keys[s_net_me].contains(f))
				return;
			Common::Timer timer;
			Error error;
			std::unique_ptr<ArchiveEntryList> list = SaveState_DownloadState(&error);
			if (!list)
			{
				Console.Error("ZdxsvGgpo: replay key %d: state download failed: %s", f, error.GetDescription().c_str());
				return;
			}
			auto key = std::make_unique<PlayKey>();
			PlayKey* k = key.get();
			k->path = Path::Combine(EmuFolders::Cache, fmt::format("zdxsv-replay-key-p{}-{}.p2s", s_net_me, f));
			k->pos = s_net_pos;
			k->rx = s_net_rx;
			std::memcpy(k->k3seen, s_zds_seen, sizeof(k->k3seen));
			k->k3rel = s_zds_rel;
			k->ps = s_ps;
			std::memcpy(k->zd_pad, s_zd_pad, sizeof(k->zd_pad));
			std::memcpy(k->zd_hist, s_zd_hist, sizeof(k->zd_hist));
			std::memcpy(k->seq_at, s_net_seq_at, sizeof(k->seq_at));
			k->zip = std::thread([list = std::move(list), k]() mutable {
				Error error;
				k->ok = SaveState_ZipToDisk(std::move(list), nullptr, k->path.c_str(), &error);
				if (!k->ok)
					Console.Error("ZdxsvGgpo: replay key: state zip failed: %s", error.GetDescription().c_str());
			});
			s_play_keys[s_net_me].emplace(f, std::move(key));
			Console.WriteLn("ZdxsvGgpo: replay key %d (download %.1f ms)", f, timer.GetTimeMilliseconds());
		}

		void PlayKeyApply(const PlayKey& k)
		{
			s_net_pos = k.pos;
			s_net_rx = k.rx;
			std::memcpy(s_zds_seen, k.k3seen, sizeof(s_zds_seen));
			s_zds_rel = k.k3rel;
			s_ps = k.ps;
			std::memcpy(s_zd_pad, k.zd_pad, sizeof(s_zd_pad));
			std::memcpy(s_zd_hist, k.zd_hist, sizeof(s_zd_hist));
			std::memcpy(s_net_seq_at, k.seq_at, sizeof(s_net_seq_at));
		}

		// Returns the frame to run next. pov_switch: s_net_me just changed, so a key of it is always loaded (the
		// running state is the old point of view's).
		int PlaySeek(int target, bool pov_switch = false)
		{
			target = std::clamp(target, 0, s_play_frames - 1);
			const int next = s_net_frame + 1;
			int from = next;
			bool loaded = false;
			auto& keys = s_play_keys[s_net_me];
			auto it = keys.upper_bound(target);
			while (it != keys.begin())
			{
				--it;
				if (!pov_switch && target >= next && it->first <= next)
					break; // running on is as close
				PlayKey& k = *it->second;
				if (k.zip.joinable())
					k.zip.join();
				if (!k.ok)
					continue;
				Common::Timer timer;
				Error error;
				if (!VMManager::LoadState(k.path.c_str(), &error))
				{
					Console.Error("ZdxsvGgpo: replay seek: key %d load failed: %s", it->first, error.GetDescription().c_str());
					return pov_switch ? -1 : next;
				}
				if (!s_play_key_nohle)
					PlayKeyApply(k);
				from = it->first;
				loaded = true;
				Console.WriteLn("ZdxsvGgpo: replay seek: key %d loaded (%.1f ms)", from, timer.GetTimeMilliseconds());
				break;
			}
			if (from > target || (pov_switch && !loaded))
			{
				Console.Error("ZdxsvGgpo: replay seek %d -> %d: no key at or before it", s_net_frame, target);
				return pov_switch ? -1 : next;
			}
			Console.WriteLn("ZdxsvGgpo: replay seek %d -> %d: from %d, %d frames to run", s_net_frame, target, from, target - from);
			if (from < target)
			{
				if (s_play_target < 0)
					s_play_limiter = VMManager::GetLimiterMode();
				s_play_target = target;
				VMManager::SetLimiterMode(LimiterModeType::Unlimited);
			}
			return from;
		}

		Input PadFromB(u16 b)
		{
			Input in = {};
			in.lx = in.ly = in.rx = in.ry = Pad::ANALOG_NEUTRAL_POSITION;
			for (u32 i = 0; i < BUTTONS; i++)
			{
				Input one = {};
				one.buttons = static_cast<u16>(1u << i);
				u16 a1, b1;
				ZdPadAB(one, a1, b1);
				if (b1 && (b & b1) == b1)
					in.buttons |= one.buttons;
			}
			return in;
		}

		// Runs unlimited until load `load` has ended (after a seek to s_play_hi, if one was started).
		void PlayRunBegin(int load)
		{
			if (s_play_target < 0) // else the seek saved it
				s_play_limiter = VMManager::GetLimiterMode();
			s_run_load = load;
			VMManager::SetLimiterMode(LimiterModeType::Unlimited);
			if (load == 1)
				Console.WriteLn("ZdxsvGgpo: replay skip MS selection: running to the briefing");
			else
				Console.WriteLn("ZdxsvGgpo: replay round %d: running to load %d", load - 1, load);
		}

		void PlayRunEnd(const char* why, int f)
		{
			if (s_run_load < 0)
				return;
			const int load = s_run_load;
			s_run_load = -1;
			if (s_play_target < 0)
				VMManager::SetLimiterMode(s_play_limiter);
			if (load == 1)
				Console.WriteLn("ZdxsvGgpo: replay skip MS selection: %s at frame %d, vsync %u", why, f, g_FrameCount);
			else
				Console.WriteLn("ZdxsvGgpo: replay round %d: %s at frame %d, vsync %u", load - 1, why, f, g_FrameCount);
		}

		void PlayTrackLoads(int f)
		{
			const u8 st = eeMem->Main[TICK_STATE];
			if (f > s_play_hi)
			{
				if (f == s_tick_f + 1 && s_tick_st == TICK_LOAD && st != TICK_LOAD)
				{
					std::lock_guard lock(s_battle_loads_mtx);
					s_battle_loads.push_back(f);
					Console.WriteLn("ZdxsvGgpo: replay: load %zu ends at frame %d, vsync %u", s_battle_loads.size() - 1, f, g_FrameCount);
				}
				s_play_hi = f;
			}
			s_tick_f = f;
			s_tick_st = st;
			if (s_run_load >= 0 && s_run_load < static_cast<int>(s_battle_loads.size()) && s_battle_loads[s_run_load] == f)
				PlayRunEnd(s_run_load == 1 ? "briefing" : "start", f);
		}

		void PlayFrame(int f)
		{
			PlayTrackLoads(f);
			if (f == s_play_target)
			{
				s_play_target = -1;
				if (s_run_load < 0)
					VMManager::SetLimiterMode(s_play_limiter);
				Console.WriteLn("ZdxsvGgpo: replay seek done at frame %d, vsync %u", f, g_FrameCount);
			}
			PlayKeySave(f);
			s_net_frame = f;
			PlayBarPublish();
			PlayKeysPublish(f);
			NetSaved(f);
			const NetInput* in = &s_play_inputs[static_cast<size_t>(f) * s_players];
			u16 ab[2];
			std::memcpy(ab, &in[s_net_me].pad, sizeof(ab));
			s_zd_pad[f & 127] = PadFromB(ab[1]);
			NetApply(in);
		}

		void PlayStop(const char* what)
		{
			g_ggpo_active = false;
			s_bar_frame = -1;
			s_net_over = true; // the battle sock goes back to the IOP
			Console.WriteLn("ZdxsvGgpo: replay %s at frame %d of %d, vsync %u", what, s_net_frame, s_play_frames, g_FrameCount);
			NetReport();
			if (const char* e = std::getenv("ZDXSV_REPLAY_EXIT"); e && e[0] == '1')
				Host::RunOnCPUThread([] { Host::RequestVMShutdown(false, false, false); });
			else if (!s_play_at_end)
				Host::RunOnCPUThread([] { VMManager::SetPaused(true); });
		}

		// Point of view: ZDXSV_REPLAY=a.zdxr;b.zdxr... = files of one battle saved by different players. Their inputs
		// are the same (checked on the common frames); each brings its own position's frame 0 state + battle-socket
		// state, kept as key 0 of that position. A switch at frame f loads the new position's newest key <= f and
		// runs to f unlimited (PlaySeek pov_switch); keys and sent msgs are kept per position.
		std::atomic<int> s_play_pov_req{-1}; // requested position, -1 = none
		std::deque<std::pair<int, int>> s_play_pov_at; // ZDXSV_REPLAY_POV_AT=frame:position,...
		struct PlaySent
		{
			std::vector<std::vector<u8>> sent;
			std::vector<int> at;
			std::deque<NetOut> out;
		};
		PlaySent s_play_sent[GGPO_MAX_PLAYERS];

		// GgpoOnVmShutdown: the key files go; a new VM (or the reset one) loads the files again and plays from the start.
		void PlayReset()
		{
			for (auto& keys : s_play_keys)
			{
				for (auto& [f, k] : keys)
				{
					if (k->zip.joinable())
						k->zip.join();
					FileSystem::DeleteFilePath(k->path.c_str());
				}
				keys.clear();
			}
			s_play_inputs.clear();
			s_play_frames = 0;
			s_play_seeks.clear();
			s_play_req = INT_MIN;
			s_play_at_end = false;
			s_play_target = -1;
			{
				std::lock_guard lock(s_battle_loads_mtx);
				s_battle_loads.clear();
			}
			s_play_hi = s_tick_f = s_run_load = -1;
			s_play_round_req = INT_MIN;
			s_play_round_at.clear();
			std::fill(std::begin(s_play_pov_ok), std::end(s_play_pov_ok), false);
			s_play_pov_req = -1;
			s_play_pov_at.clear();
			std::fill(std::begin(s_play_sent), std::end(s_play_sent), PlaySent{});
			s_bar_frame = -1;
			s_bar_frames = 0;
			s_bar_pov = s_bar_target = -1;
			s_bar_povs = 0;
		}

		// Reads one file; returns its position, -1 = not used.
		int PlayLoadFile(const std::string& path)
		{
			const std::optional<std::vector<u8>> file = FileSystem::ReadBinaryFile(Path::ToNativePath(path).c_str()); // '/' fails on Windows
			const std::string_view all = file ? std::string_view(reinterpret_cast<const char*>(file->data()), file->size()) : std::string_view();
			const size_t end = all.find("\n\n");
			if (!all.starts_with("ZDXSV-REPLAY 1\n") || end == std::string_view::npos)
			{
				Console.Error("ZdxsvGgpo: replay %s: not a replay file", path.c_str());
				return -1;
			}
			std::map<std::string, std::string, std::less<>> kv;
			for (const std::string_view line : StringUtil::SplitString(all.substr(0, end), '\n'))
				if (const size_t eq = line.find('='); eq != std::string_view::npos)
					kv.emplace(line.substr(0, eq), line.substr(eq + 1));
			const auto num = [&kv](std::string_view k) -> s64 {
				const auto it = kv.find(k);
				return it == kv.end() ? -1 : StringUtil::FromChars<s64>(it->second).value_or(-1);
			};
			const s64 players = num("players"), me = num("position"), frames = num("frames"), state_size = num("state_size");
			const size_t data = end + 2;
			// each bound before the next product: header values are any s64
			if (players < 1 || players > GGPO_MAX_PLAYERS || me < 0 || me >= players || frames < 1 || frames > INT_MAX ||
				state_size <= 0 || num("input_size") != static_cast<s64>(sizeof(NetInput)) ||
				static_cast<u64>(state_size) > all.size() - data ||
				static_cast<u64>(frames) > (all.size() - data - state_size) / (players * sizeof(NetInput)))
			{
				Console.Error("ZdxsvGgpo: replay %s: bad header or short file (players %lld position %lld frames %lld)", path.c_str(),
					players, me, frames);
				return -1;
			}
			const bool zds_ps = num("zds_ps") == 1;
			const NetInput* in = reinterpret_cast<const NetInput*>(all.data() + data + state_size);
			if (s_play_frames > 0)
			{
				// a later file: same battle, another position
				const size_t common = static_cast<size_t>(std::min<s64>(frames, s_play_frames)) * players;
				const auto diff = std::mismatch(in, in + common, s_play_inputs.begin(), [](const NetInput& a, const NetInput& b) {
					return std::memcmp(&a, &b, sizeof(a)) == 0;
				});
				if (players != s_players || zds_ps != s_zds_ps || s_play_pov_ok[me] || diff.first != in + common)
				{
					Console.Error("ZdxsvGgpo: replay %s: not another position of the first file's battle (players %lld/%d, zds_ps %d/%d, "
								  "position %lld %s, inputs differ from frame %lld)",
						path.c_str(), players, s_players, zds_ps ? 1 : 0, s_zds_ps ? 1 : 0, me, s_play_pov_ok[me] ? "taken" : "new",
						diff.first != in + common ? static_cast<s64>((diff.first - in) / players) : -1);
					return -1;
				}
			}
			auto key = std::make_unique<PlayKey>();
			const auto rx0 = kv.find("rx0");
			if (rx0 != kv.end())
				for (size_t i = 0; i + 1 < rx0->second.size(); i += 2)
					key->rx.push_back(static_cast<u8>(std::strtoul(rx0->second.substr(i, 2).c_str(), nullptr, 16)));
			int h[9] = {};
			const auto hle0 = kv.find("hle0");
			if (rx0 == kv.end() || hle0 == kv.end() ||
				std::sscanf(hle0->second.c_str(), "%d,%d,%d,%d,%d,%d,%d,%d,%d", &h[0], &h[1], &h[2], &h[3], &h[4], &h[5], &h[6], &h[7], &h[8]) != 9)
				Console.Warning("ZdxsvGgpo: replay: no rx0 / hle0 (file from before replay play): frame 0 HLE state empty");
			// The kind-3 counters index s_zds_k3 (NetApply) and the file holds no kind-3 bodies: only the empty barrier
			// the recorder writes (ReplayBegin, right after the session reset) can be played.
			if (std::any_of(h + 4, h + 9, [](int v) { return v != 0; }))
			{
				Console.Error("ZdxsvGgpo: replay %s: hle0 kind-3 counters %d,%d,%d,%d,%d not 0", path.c_str(), h[4], h[5], h[6], h[7], h[8]);
				return -1;
			}
			const std::string state_path = Path::Combine(EmuFolders::Cache, fmt::format("zdxsv-replay-p{}.p2s", me));
			if (!FileSystem::WriteBinaryFile(state_path.c_str(), all.data() + data, state_size))
			{
				Console.Error("ZdxsvGgpo: replay: cannot write %s", state_path.c_str());
				return -1;
			}
			if (frames > s_play_frames)
			{
				s_play_inputs.assign(in, in + frames * players);
				s_play_frames = static_cast<int>(frames);
			}
			s_players = static_cast<int>(players);
			s_zds_ps = zds_ps;
			key->path = state_path;
			key->ps = {static_cast<u8>(h[0]), static_cast<u8>(h[1]), h[2] != 0, h[3] != 0};
			for (int p = 0; p < 4; p++)
				key->k3seen[p] = h[4 + p];
			key->k3rel = h[8];
			key->ok = true;
			Console.WriteLn("ZdxsvGgpo: replay %s: position %lld of %d, %lld frames, zds_ps %d, rx0 %zu bytes, state %lld bytes", path.c_str(),
				me, s_players, frames, s_zds_ps ? 1 : 0, key->rx.size(), state_size);
			s_play_keys[me].emplace(0, std::move(key));
			s_play_pov_ok[me] = true;
			return static_cast<int>(me);
		}

		// CPU thread, queued at the first vsync.
		void PlayLoad()
		{
			int me = -1;
			for (const std::string_view path : StringUtil::SplitString(s_play_env, ';'))
				if (const int p = PlayLoadFile(std::string(path)); me < 0)
					me = p;
			if (me < 0)
				return;
			if (const char* e = std::getenv("ZDXSV_REPLAY_POV"))
			{
				const int p = std::atoi(e);
				if (p >= 0 && p < s_players && s_play_pov_ok[p])
					me = p;
				else
					Console.Error("ZdxsvGgpo: replay: ZDXSV_REPLAY_POV=%s has no file, playing position %d", e, me);
			}
			const PlayKey& k0 = *s_play_keys[me].at(0);
			Error error;
			g_ps_hook = s_zds_ps; // before the load: the recompiler cache is rebuilt from it
			if (!VMManager::LoadState(k0.path.c_str(), &error))
			{
				Console.Error("ZdxsvGgpo: replay: state load failed: %s", error.GetDescription().c_str());
				return;
			}
			s_net = true;
			s_net_me = me;
			PlayKeyApply(k0);
			s_net_armed = true;
			s_started = true;
			g_ggpo_active = true;
			if (const char* e = std::getenv("ZDXSV_REPLAY_TURBO"); e && e[0] == '1')
				VMManager::SetLimiterMode(LimiterModeType::Turbo);
			if (s_play_skip_ms)
				PlayRunBegin(1);
			if (const char* e = std::getenv("ZDXSV_REPLAY_ROUND_AT"))
				for (const std::string_view one : StringUtil::SplitString(e, ','))
					if (const size_t c = one.find(':'); c != std::string_view::npos)
						s_play_round_at.emplace_back(StringUtil::FromChars<int>(one.substr(0, c)).value_or(-1),
							StringUtil::FromChars<int>(one.substr(c + 1)).value_or(-1));
			if (const char* e = std::getenv("ZDXSV_REPLAY_SEEK"))
				for (const std::string_view one : StringUtil::SplitString(e, ','))
					if (const size_t c = one.find(':'); c != std::string_view::npos)
						s_play_seeks.emplace_back(StringUtil::FromChars<int>(one.substr(0, c)).value_or(-1),
							StringUtil::FromChars<int>(one.substr(c + 1)).value_or(0));
			if (const char* e = std::getenv("ZDXSV_REPLAY_POV_AT"))
				for (const std::string_view one : StringUtil::SplitString(e, ','))
					if (const size_t c = one.find(':'); c != std::string_view::npos)
						s_play_pov_at.emplace_back(StringUtil::FromChars<int>(one.substr(0, c)).value_or(-1),
							StringUtil::FromChars<int>(one.substr(c + 1)).value_or(-1));
			std::string povs;
			for (int p = 0; p < s_players; p++)
				if (s_play_pov_ok[p])
					povs += fmt::format("{}{}", povs.empty() ? "" : ",", p);
			Console.WriteLn("ZdxsvGgpo: replay: point of view %d (positions with a file: %s), %d frames", s_net_me, povs.c_str(), s_play_frames);
			PlayFrame(0);
		}

		// Returns the frame to run next.
		int PlaySwitch(int pov, int target)
		{
			if (pov < 0 || pov >= s_players || !s_play_pov_ok[pov])
			{
				Console.Error("ZdxsvGgpo: replay: no file for point of view %d", pov);
				return s_net_frame + 1;
			}
			const int old = s_net_me;
			const auto swap_sent = [](int p) {
				std::swap(s_net_sent, s_play_sent[p].sent);
				std::swap(s_net_sent_at, s_play_sent[p].at);
				std::swap(s_net_out, s_play_sent[p].out);
			};
			swap_sent(old); // park the old position's sent msgs
			swap_sent(pov);
			s_net_me = pov;
			Console.WriteLn("ZdxsvGgpo: replay: point of view %d -> %d at frame %d, vsync %u", old, pov, target, g_FrameCount);
			const int next = PlaySeek(target, true);
			if (next >= 0)
				return next;
			swap_sent(pov);
			swap_sent(old);
			s_net_me = old;
			return s_net_frame + 1;
		}

		void PlayNext()
		{
			s_session_frames++;
			int next = s_net_frame + 1;
			if (!s_play_seeks.empty() && s_play_seeks.front().first == next)
			{
				s_play_req = s_play_seeks.front().second;
				s_play_seeks.pop_front();
			}
			if (!s_play_pov_at.empty() && s_play_pov_at.front().first == next)
			{
				s_play_pov_req = s_play_pov_at.front().second;
				s_play_pov_at.pop_front();
			}
			if (!s_play_round_at.empty() && s_play_round_at.front().first == next)
			{
				s_play_round_req = s_play_round_at.front().second;
				s_play_round_at.pop_front();
			}
			int req = s_play_req.exchange(INT_MIN);
			const int pov = s_play_pov_req.exchange(-1);
			int run = -1; // load to run to after the seek
			if (const int round = s_play_round_req.exchange(INT_MIN); round != INT_MIN)
			{
				const int load = 1 + std::max(0, round);
				if (load < static_cast<int>(s_battle_loads.size()))
				{
					req = s_battle_loads[load];
					Console.WriteLn("ZdxsvGgpo: replay round %d: starts at frame %d", load - 1, req);
				}
				else
				{
					req = s_play_hi > s_net_frame ? s_play_hi : INT_MIN;
					run = load;
				}
			}
			if (req != INT_MIN || run >= 0 || (pov >= 0 && pov != s_net_me))
				PlayRunEnd("cancelled by a seek", s_net_frame);
			if (req == 0 && s_battle_loads.size() > 1)
				req = s_battle_loads[1]; // from the start = from the briefing
			if (pov >= 0 && pov != s_net_me)
				next = PlaySwitch(pov, req != INT_MIN ? req : next);
			else if (req != INT_MIN)
				next = PlaySeek(req);
			if (req == 0 && next == 0 && s_play_skip_ms)
				run = 1; // briefing not reached yet
			if (run >= 0)
				PlayRunBegin(run);
			if (next >= s_play_frames)
				PlayRunEnd("replay ended first", s_net_frame);
			if (next < s_play_frames)
			{
				s_play_at_end = false;
				PlayFrame(next);
			}
			else if (const char* e = std::getenv("ZDXSV_REPLAY_EXIT"); s_play_at_end || (e && e[0] == '1'))
				PlayStop("end");
			else
			{
				// a seek requested while paused here plays on; resuming without one stops the replay
				s_play_at_end = true;
				Console.WriteLn("ZdxsvGgpo: replay at its end (frame %d of %d, vsync %u), paused", s_net_frame, s_play_frames, g_FrameCount);
				VMManager::SetPaused(true); // now: queued, one more frame ran and ended the replay (s709)
			}
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
					if (const char* t = Zdxsv::TestEnv("ZDXSV_RBK_TIME"))
					{
						const int s = std::atoi(t);
						b[RBK_RULE_TIME] = static_cast<u8>(s >> 8);
						b[RBK_RULE_TIME + 1] = static_cast<u8>(s);
					}
					if (const char* g = Zdxsv::TestEnv("ZDXSV_RBK_GAUGE"))
					{
						const int v = std::atoi(g);
						for (size_t o : {RBK_RULE_GAUGE, RBK_RULE_GAUGE + 2})
						{
							b[o] = static_cast<u8>(v >> 8);
							b[o + 1] = static_cast<u8>(v);
						}
					}
					if (const char* c = Zdxsv::TestEnv("ZDXSV_RBK_COUNT"))
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
		// lobby=1, at the battle's first key msg: GGPO only when the battle info has every peer and our
		// position; else the battle stays on the battle server (logged once per battle info).
		bool LobbyArm(int me)
		{
			std::lock_guard lock(s_lobby_mtx);
			const int n = static_cast<int>(s_lobby_peers.size());
			const char* why = !s_lobby_info ? "no battle info" :
			                  !s_lobby_ok ? "a peer has no GGPO address" :
			                  s_lobby_unreachable ? "a peer did not answer the ping test" :
			                  !s_lobby_session ? "no ggpo_session" :
			                  (n < 2 || n > GGPO_MAX_PLAYERS) ? "player count" :
			                  (me < 0 || me >= n || !s_lobby_peers[me].empty()) ? "own position not in the battle info" :
			                                                                         nullptr;
			auto report = [&](const char* result) {
				s_report = s_report_ids + "result=" + result + "\nposition=" + std::to_string(me) + "\nplayers=" + std::to_string(n) + "\n";
			};
			if (why)
			{
				if (!s_lobby_logged)
				{
					Console.WriteLn("ZdxsvGgpo: lobby battle stays on the battle server: %s (position %d, %d players)", why, me, n);
					if (s_lobby_info) // else not a lobby battle (s696: key msgs before the login)
					{
						report("server");
						s_report += std::string("reason=") + why + "\n";
					}
				}
				s_lobby_logged = true;
				return false;
			}
			std::vector<Zdxsv::PeerAddr> peers(n);
			for (int p = 0; p < n; p++)
				if (!s_lobby_peers[p].empty())
					peers[p] = s_lobby_peers[p].front();
			if (!s_delay_set)
			{
				// as flycast's rollback backend: one-way time to the slowest peer in 16 ms frames, rounded up
				Common::Timer wait;
				std::vector<Zdxsv::RelayServerAddr> servers;
				std::string pingError;
				const std::vector<Zdxsv::PingResult> pings = Zdxsv::FinishPingTest(&servers, &pingError);
				if (!pingError.empty())
					Console.WriteLn("ZdxsvGgpo: lobby ping test failed: %s", pingError.c_str());
				std::vector<int> via(n);
				int rtt = -1, up = 0;
				for (size_t p = 0; p < pings.size() && p < peers.size(); p++)
				{
					if (pings[p].rtt <= 0)
						continue;
					rtt = std::max(rtt, pings[p].rtt), up++;
					peers[p] = pings[p].addr; // the address the ping test picked (IPv4 or IPv6), or the relay's
					via[p] = pings[p].via;
				}
				std::string rtts;
				for (size_t p = 0; p < pings.size(); p++)
					if (static_cast<int>(p) != me)
					{
						const std::string path = pings[p].via == 1 ? "peer " + std::to_string(pings[p].relay) :
						                         pings[p].via == 2 ? "relay " + std::to_string(pings[p].relay) : "direct";
						rtts += "rtt_" + std::to_string(p) + "=" + std::to_string(pings[p].rtt) +
						        (pings[p].rtt > 0 ? "\naddr_" + std::to_string(p) + "=" + pings[p].addr.String() + "\npath_" +
						                                std::to_string(p) + "=" + path : "") + "\n";
						if (pings[p].rtt > 0)
							Console.WriteLn("ZdxsvGgpo: lobby path to position %zu: %s, rtt %d ms, at %s", p, path.c_str(),
								pings[p].rtt, pings[p].addr.String().c_str());
					}
				rtts += "relays=" + std::to_string(servers.size()) + "\n";
				if (!pingError.empty())
					rtts += "ping_error=" + pingError + "\n";
				s_net_via = std::move(via);
				s_net_servers = std::move(servers);
				rtts += "ping_wait_ms=" + std::to_string(static_cast<int>(wait.GetTimeMilliseconds())) + "\n";
				// as flycast ("Peer%d unreachable"): no GGPO unless every peer answered the ping test (same
				// session, position and address); a peer from another battle never gets our inputs
				if (up < n - 1)
				{
					Console.WriteLn("ZdxsvGgpo: lobby battle connection cut: %d of %d peers answered the ping test (position %d, waited %.1f s)",
						up, n - 1, me, wait.GetTimeSeconds());
					s_lobby_unreachable = s_lobby_logged = s_lobby_cut = true;
					s_cut_sends = 0;
					report("cut");
					s_report += "answered=" + std::to_string(up) + "\n" + rtts;
					return false;
				}
				s_delay = std::max(s_min_delay, rtt > 0 ? (rtt + 31) / 32 : 0);
				Console.WriteLn("ZdxsvGgpo: lobby delay %d: slowest peer rtt %d ms (%d of %d peers measured), min %d, waited %.1f s for the ping test",
					s_delay, rtt, up, n - 1, s_min_delay, wait.GetTimeSeconds());
				report("ggpo");
				s_report += "delay=" + std::to_string(s_delay) + "\nmin_delay=" + std::to_string(s_min_delay) + "\n" + rtts;
			}
			else
			{
				report("ggpo");
				s_report += "delay=" + std::to_string(s_delay) + "\nfixed_delay=1\n";
				s_net_via.clear();
				s_net_servers.clear();
			}
			s_players = n;
			s_net_peers = std::move(peers);
			s_net_players = s_lobby_players;
			s_armed_gen = s_lobby_gen;
			return true;
		}
		// lobby=1, the ping test failed (LobbyArm): a connection failure, no fallback to the battle server.
		// The battle sock goes silent: sends are dropped, nothing to recv. The game gets no response, gives up
		// and reconnects to the lobby; its next connect or close goes to the IOP and ends the cut.
		bool LobbyCutCall(u32 fno, s16 sock, s16 len)
		{
			u8* ram = eeMem->Main;
			s32 result = 0;
			if (sock == NET_BATTLE_SOCK && fno == NET_FNO_SEND)
				result = std::clamp<s32>(len, 0, 0x3ca), s_cut_sends++;
			else if (sock == NET_BATTLE_SOCK && fno == NET_FNO_POLL)
			{
				*reinterpret_cast<u16*>(ram + NET_REQ_LEN) = 4;
				*reinterpret_cast<u16*>(ram + NET_REQ_DATA + 2) = 0x2000;
				*reinterpret_cast<u16*>(ram + NET_REQ_DATA + 4) = 0;
			}
			else if (!(sock == NET_BATTLE_SOCK && fno == NET_FNO_RECV))
			{
				if (fno == 7 || fno == 0xd)
				{
					Console.WriteLn("ZdxsvGgpo: lobby battle connection cut ended at vsync %u: game RPC 0x%x after %u dropped sends",
						g_FrameCount, fno, s_cut_sends);
					s_lobby_cut = false;
					std::lock_guard lock(s_lobby_mtx);
					if (!s_report.empty())
						s_report += "cut_sends=" + std::to_string(s_cut_sends) + "\n";
				}
				return false;
			}
			*reinterpret_cast<s32*>(ram + NET_RES_LEN) = result;
			cpuRegs.GPR.n.v0.SD[0] = result;
			cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
			return true;
		}
	} // namespace

	int GgpoLobbyPort()
	{
		// s_options only changes in GgpoOnVmInitialize, before the DEV9 thread that calls this runs
		if (s_options.find("net=1") == std::string::npos || s_options.find("lobby=1") == std::string::npos)
			return 0;
		int p = 7001;
		for (const std::string_view item : StringUtil::SplitString(s_options, ','))
			if (item.starts_with("port="))
				p = StringUtil::FromChars<int>(item.substr(5)).value_or(0);
		return (p > 0 && p <= 0xFFFF) ? p : 0;
	}

	void ReplaySeekBy(int frames)
	{
		if (!s_play_env || s_play_frames <= 0 || !g_ggpo_active)
			return;
		const int req = s_play_req.load();
		s_play_req = (req != INT_MIN ? req : s_net_frame + 1) + frames;
		Console.WriteLn("ZdxsvGgpo: replay seek requested: frame %d", s_play_req.load());
		if (s_play_at_end && VMManager::GetState() == VMState::Paused)
			VMManager::SetPaused(false);
	}

	void ReplayNextPov()
	{
		if (!s_play_env || s_play_frames <= 0 || !g_ggpo_active)
			return;
		const int req = s_play_pov_req.load();
		int p = req >= 0 ? req : s_net_me;
		for (int i = 0; i < s_players; i++)
			if (p = (p + 1) % s_players; s_play_pov_ok[p])
				break;
		if (p == s_net_me)
		{
			Console.WriteLn("ZdxsvGgpo: replay: no other point of view (one file per position: ZDXSV_REPLAY=a.zdxr;b.zdxr)");
			return;
		}
		s_play_pov_req = p;
		Console.WriteLn("ZdxsvGgpo: replay point of view requested: %d", p);
		if (s_play_at_end && VMManager::GetState() == VMState::Paused)
			VMManager::SetPaused(false);
	}

	void ReplaySeekTo(int frame)
	{
		if (!s_play_env || s_play_frames <= 0 || !g_ggpo_active)
			return;
		s_play_req = std::clamp(frame, 0, s_play_frames - 1);
		Console.WriteLn("ZdxsvGgpo: replay seek requested: frame %d", s_play_req.load());
		if (VMManager::GetState() == VMState::Paused) // control bar: a seek plays on, also from a pause
			VMManager::SetPaused(false);
	}

	void ReplayTogglePause()
	{
		if (!s_play_env || s_play_frames <= 0 || !g_ggpo_active)
			return;
		if (s_play_at_end)
			ReplaySeekTo(0); // play at the end = from the start
		else
			VMManager::SetPaused(VMManager::GetState() != VMState::Paused);
	}

	bool ReplayBarInfo(int& frame, int& frames, int& pov, u32& povs, int& target)
	{
		frame = s_bar_frame;
		if (!s_play_env || frame < 0)
			return false;
		frames = s_bar_frames;
		pov = s_bar_pov;
		povs = s_bar_povs;
		target = s_bar_target;
		return true;
	}

	bool ReplayKeys(std::vector<std::pair<u16, int>>& runs)
	{
		if (!s_keys_on || s_bar_frame < 0)
			return false;
		std::lock_guard lock(s_keys_mtx);
		runs.assign(s_keys.begin(), s_keys.end());
		return true;
	}

	void ReplayToggleKeys()
	{
		if (!s_play_env || s_play_frames <= 0 || !g_ggpo_active)
			return;
		s_keys_on = !s_keys_on;
		Console.WriteLn("ZdxsvGgpo: replay key display %s", s_keys_on ? "on" : "off");
		{
			std::lock_guard lock(s_keys_mtx);
			s_keys_f = -1; // rebuilt now: a paused replay plays no frame
		}
		PlayKeysPublish(s_net_frame);
	}

	void ReplayJumpRound(int delta)
	{
		if (!s_play_env || s_play_frames <= 0 || !g_ggpo_active)
			return;
		int round = s_play_round_req.load();
		if (round == INT_MIN)
		{
			round = 0; // rounds started at or before the shown frame
			const int f = s_play_target >= 0 ? s_play_target : s_net_frame;
			for (size_t k = 2; k < s_battle_loads.size() && s_battle_loads[k] <= f; k++)
				round++;
		}
		s_play_round_req = std::max(0, round + delta);
		Console.WriteLn("ZdxsvGgpo: replay round requested: %d", s_play_round_req.load());
		if (VMManager::GetState() == VMState::Paused)
			VMManager::SetPaused(false);
	}

	std::vector<int> ReplayRoundStarts()
	{
		std::lock_guard lock(s_battle_loads_mtx);
		return s_battle_loads.size() > 2 ? std::vector<int>(s_battle_loads.begin() + 2, s_battle_loads.end()) : std::vector<int>();
	}

	int GgpoLobbyAdvertisePort()
	{
		if (GgpoLobbyPort() <= 0)
			return 0;
		int p = 0;
		for (const std::string_view item : StringUtil::SplitString(s_options, ','))
			if (item.starts_with("advertise="))
				p = StringUtil::FromChars<int>(item.substr(10)).value_or(0);
		return (p > 0 && p <= 0xFFFF) ? p : 0;
	}

	void SetLobbyPeers(bool ok, std::vector<std::vector<Zdxsv::PeerAddr>> byPosition, u32 session, int pingMs, std::string ids,
		std::vector<std::pair<std::string, std::string>> players, std::vector<Zdxsv::BattleInfo::Relay> relays)
	{
		std::lock_guard lock(s_lobby_mtx);
		s_report_ids = std::move(ids);
		s_lobby_players = std::move(players);
		s_lobby_info = true;
		s_lobby_gen++;
		s_lobby_ok = ok;
		s_lobby_logged = s_lobby_unreachable = false;
		if (s_bad_session && session)
		{
			session ^= 0x5a5a5a5a;
			Console.WriteLn("ZdxsvGgpo: badsession=1: ping test session %08x", session);
		}
		s_lobby_session = session;
		s_lobby_peers = std::move(byPosition);
		s_lobby_relays = std::move(relays);
		if (ok && session && !s_delay_set && pingMs > 0)
			Zdxsv::StartPingTest(session, s_lobby_peers, static_cast<u16>(GgpoLobbyPort()), pingMs, s_lobby_relays);
	}

	std::string TakeLobbyReport()
	{
		std::lock_guard lock(s_lobby_mtx);
		return std::exchange(s_report, {});
	}

	std::vector<GgpoOsdLine> GgpoOsdLines()
	{
		std::lock_guard lock(s_osd_mtx);
		return s_osd_lines;
	}

	// EE rec hook at NET_RPC_PC (the net RPC wrapper's entry): trace, and in net mode answer the
	// battle sock's RPCs here. Returns true when answered (v0 = result, pc = ra).
	bool OnNetCall()
	{
		OnNetRpc();
		if (!s_net_env)
			return false;
		const u32 fno = cpuRegs.GPR.n.a0.UL[0];
		u8* ram = eeMem->Main;
		const s16 sock = *reinterpret_cast<const s16*>(ram + NET_REQ_SOCK);
		const s16 len = *reinterpret_cast<const s16*>(ram + NET_REQ_LEN);
		u8* d = ram + NET_REQ_DATA;
		const bool key = fno == NET_FNO_SEND && sock == NET_BATTLE_SOCK && len > 0 && len <= 0x3ca && HasKeyMsg(d, len);
		if (s_net_over)
		{
			// lobby=1: the first key msg of a battle with a new battle info starts the next GGPO battle; the
			// sends of the ended battle (same battle info) stay with the IOP
			if (!s_lobby || s_play_env || s_rbk || !key || !LobbyNewBattle())
				return false;
			NetReset();
			Console.WriteLn("ZdxsvGgpo: next lobby battle at vsync %u", g_FrameCount);
		}
		if (s_rbk && !s_net_armed && !key) // connect (fno 7) has the address in the sock field
			return RbkCall(fno, len, d);
		if (s_lobby_cut)
			return LobbyCutCall(fno, sock, len);
		if (sock != NET_BATTLE_SOCK)
			return false;
		if (!s_net_armed)
		{
			// sock 0 is the lobby TCP too: arm at the battle's first key msg
			if (!key)
				return false;
			int me = -1;
			for (s32 i = 0; i + 1 < len && d[i] >= 2 && i + d[i] <= len; i += d[i])
				if ((d[i + 1] >> 4) == 2)
					me = d[i + 1] & 0xf;
			if (s_lobby && !LobbyArm(me))
				return s_lobby_cut && LobbyCutCall(fno, sock, len);
			s_net_armed = true;
			if (s_rbk)
			{
				s_net_rx.insert(s_net_rx.end(), s_rbk_rx.begin(), s_rbk_rx.end());
				s_rbk_rx.clear();
				// ZDXSV_RBK_TURBO=1: the battle runs turbo too (GGPO paces the peers by frame)
				if (!Zdxsv::TestEnv("ZDXSV_RBK_TURBO"))
					VMManager::SetLimiterMode(LimiterModeType::Nominal);
			}
			if (me >= 0)
				s_net_me = me;
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
} // namespace Zdxsv

namespace Zdxsv
{
	// EE probe (iR5900.cpp): GGPO frame being run, the frame numbers of NET_TRACE H lines and PW dumps.
	int ProbeFrame() { return s_net_frame; }

	void GgpoOnVmShutdown(const char* what)
	{
		if (std::strcmp(what, "vm shutdown") == 0)
			g_mtvu_off = false; // the next VM decides again (a reset VM keeps it off)
		if (!g_ggpo_enabled)
			return;
		size_t keys = 0;
		for (const auto& k : s_play_keys)
			keys += k.size();
		Console.WriteLn("ZdxsvGgpo: %s: session %d, replay recording %d, replay keys %zu", what, g_ggpo_active && !s_play_env ? 1 : 0,
			s_replay_state.empty() ? 0 : 1, keys);
		if (s_play_env)
		{
			g_ggpo_active = false;
			PlayReset();
		}
		else if (g_ggpo_active)
		{
			s_vm_closing = true;
			Stop(what); // peers see a disconnect; the replay is saved up to the last confirmed frame
			s_vm_closing = false;
		}
		Zdxsv::DeltaStateClear();
		NetReset();
		g_ggpo_in_rollback = false;
		s_vsyncs = 0; // the next VM start or the reset VM parses the options again (and loads a replay again)
	}
} // namespace Zdxsv
