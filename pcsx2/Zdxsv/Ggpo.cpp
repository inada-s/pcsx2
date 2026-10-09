// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// GGPO for lobby battles of the Z game (DEFAULT_OPTIONS) is on by default: setting DEV9/Eth ZdxsvGgpo.
// ZDXSV_GGPO replaces it (GgpoOnVmInitialize); ZDXSV_GGPO=0 = off.
// ZDXSV_GGPO="key=value,...": a GGPO session in a running game. Synctest by default:
// every frame is saved, and every `check` frames GGPO loads the frame `check` back, reruns the
// frames with the same inputs and compares the state checksums (hash=).
// With net=1 a battle of players= peers instead (see NetInput): the session starts when the game
// arms its battle sock, every peer runs its own position, no state hashes.
//   net=1        GGPO battle session (tests/zdxsv/rbk.sh, m4relay.sh)
//   players=4    peers (2..4); GGPO player = battle position + 1
//   port=7001    UDP port of position 0; position p listens on port + p, peers on host= (default 127.0.0.1)
//   relay=R      remote p is at R + 8 * me + p (tools/zdxsv/udprelay.py per pair)
//   lobby=1      battles from the zdxsv lobby: platform info announces ggpo=port, the lobby's battle
//                info gives players and peer addresses; listen on port itself. A peer without GGPO or
//                one that did not answer the ping test: connection failure, no fallback to the battle
//                server (LobbyCutCall).
//   delay=0      GGPO frame delay of the local input (fixed). Without it a lobby battle picks
//                max(mindelay, ceil(slowest peer's rtt / 2 / 16 ms)) when GGPO arms; rtt from a ping
//                test on the GGPO port (flycast UdpPingPong packets, Zdxsv::StartPingTest)
//   mindelay=2   lower bound of that pick
//   badsession=1 test: this client's ping test uses another session id, so no peer answers it (the cut)
//   advertise=P  lobby test: the platform info announces 127.0.0.1 and GGPO port P only (no STUN /
//                local / IPv6 address), so peers reach us through a localhost tools/zdxsv/udprelay.py at P
//   replay=DIR   net: save the battle to DIR/<battle_code>.pb (frame 0 state + all inputs, ReplayWrite);
//                lobby=1 saves to <data dir>/replays by default; replay=0 = off
//   upload=URL   lobby=1: post the saved replay to this uploader (zdxsv infra/uploader) instead of the setting
//                DEV9/Eth ZdxsvReplayUploadUrl (empty = no upload)
//   osd=1        net: network status OSD (GgpoOsdLines; 0 = off); its text is also logged every 600 frames
//   sync=0       no state hashes: checksum 0 (net: always)
//   start=1500   vsync (counted from boot) the session starts at
//   frames=3000  frames the session runs, then it is closed and reported
//   check=6      synctest check distance (1..6)
//   hash=pw      synctest checksum: the player work of all 4 players, masked as the H lines (PwHash),
//                and the game RNG words (without them a rerun with other inputs can go unseen);
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
#include "Zdxsv/DeltaFreeze.h"
#include "Zdxsv/Lobby.h"
#include "Zdxsv/Proto.h"
#include "Zdxsv/SpectateSync.h"
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
#include "common/HTTPDownloader.h"
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
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#include <limits>

namespace Zdxsv
{
	// ZDXSV_REPLAY=file.pb: plays a saved replay (PlayLoad), the net=1 hooks on, no GGPO session
	const char* const s_play_env = std::getenv("ZDXSV_REPLAY");
	// Common start: the replay has no start state (or ZDXSV_REPLAY_COMMON=1). The booted state (any post-entry
	// state, tests/zdxsv/rbkprep.sh) plays the battle start with the file's lobby answers through RbkCall up to the
	// arm, which is GGPO frame 0 (PlayCommonStart).
	bool s_play_common = false;
	bool g_ggpo_enabled = false; // GgpoOnVmInitialize
	bool g_mtvu_off = false; // GgpoOnVmInitialize, cleared at VM shutdown
	s32 g_frame_period_trim_us = 0;
	namespace
	{
		// The ZDXSV_GGPO options of this VM: the variable, else DEFAULT_OPTIONS for the Z game with the
		// ZdxsvGgpo setting on, else empty = off. Set by GgpoOnVmInitialize before the CPU runs.
		std::string s_options;
		constexpr const char* GAME_SERIAL = "SLPS-25419";
		constexpr u32 GAME_CRC = 0x435D8236; // ELF CRC of SLPS_254.19: the hooks' fixed guest addresses are this build's
		constexpr const char* DEFAULT_OPTIONS = "net=1,lobby=1";
		// The hosted post-entry save state a replay or live battle starts from (PlayCommonState)
		constexpr const char* REPLAY_STATE_URL = "https://storage.googleapis.com/zdxsv/misc/rbk-p1.p2s";
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

		// net=1: the Z battle runs over GGPO, game-side input delay 0.
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
		// ZDXSV_RBK=i/N (net=1; flycast rbk_test): started from a post-entry state (tests/zdxsv/rbkprep.sh)
		// as battle position i (0-based) of N. Until GGPO arms, every lobby / battle connect RPC is
		// answered here (RbkCall: built-in battle start, recorded connect results, own battle msgs
		// echoed per remote position) and the limiter runs turbo; the process exits at the session end.
		// ZDXSV_RBK_TIME=s: rule time limit (recorded 210). ZDXSV_RBK_COUNT=n: battles (recorded 0 =
		// rematch by input). ZDXSV_RBK_GAUGE=v: 戦力ゲージ (recorded 600). ZDXSV_RAND_INPUT=seed: pad input.
		int s_rbk_me = -1, s_rbk_n = 0;
		bool s_rbk = [] { // also set by a replay's common start (PlayLoad)
			const char* e = Zdxsv::TestEnv("ZDXSV_RBK");
			return e && std::sscanf(e, "%d/%d", &s_rbk_me, &s_rbk_n) == 2 && s_rbk_me >= 0 && s_rbk_me < s_rbk_n && s_rbk_n <= 4;
		}();
		const char* s_rand_env = Zdxsv::TestEnv("ZDXSV_RAND_INPUT");
		bool s_net = false; // net=1 parsed
		int s_players = 4; // players= (net)
		int s_relay = 0; // relay=R (net): remote p is at port R + 8 * me + p (tools/zdxsv/udprelay.py per pair), not port + p
		u32 s_zds_echo = 0, s_zds_skip = 0;
		// ZDXSV_PW_HASH=1: the sync check: per GGPO frame (last save wins)
		// each player's coordinates and the game RNG, written as `H frame h0 h1 h2 h3 rng` to NET_TRACE at
		// the report: h<p> = XXH3 of x, y, z (3 floats at player work 0x8395d8 + 0x2200*p + 0x2a8), rng =
		// u16 0x6d7940 (generator 0x20f4b0, the DC games' x*3>>8 byte RNG) << 16 | u16 0x6d793c (generator
		// 0x20f4f0, s*176 % 32749); both seeded by 0x20f490. Every machine simulates every player, so in sync
		// the coordinates and RNG B agree across peers; RNG A also takes machine-local draws (sound pick at
		// 0x23abec), so it differs in sync and tools/zdxsv/pwcheck.py only reports it.
		const bool s_pw_hash = [] {
			const char* e = Zdxsv::TestEnv("ZDXSV_PW_HASH");
			return e && e[0] == '1';
		}();
		constexpr u32 PW_BASE = 0x8395d8, PW_SIZE = 0x2200;
		// Left out of the hash: machine-local 1-frame scratch, seen in an in-sync battle:
		// +0x274/+0x2b4 go 1.0 -> a different float per machine -> 0 at one frame on all machines;
		// +0x1e64..+0x1e94 (stride 0x10) set on one machine for one frame. Player work agrees after.
		// +0x214c = player struct (0x839330 + 0x2200*q) +0x1f4 of q = p + 1: HUD gauge display value, moved 1/frame
		// toward +0x1f2 by 0x14d860 for the own position only (the store is at 0x14d910).
		constexpr u32 PW_MASK[] = {0x274, 0x2b4, 0x1e64, 0x1e74, 0x1e84, 0x1e94, 0x214c};
		// Viewer-team bits: equal on the machines of one side, set for the
		// other side's players: +0x68 0x300 (119 frames mid-battle), and from time-up on +0x58 0x100,
		// +0x9c 0x10000, +0x2068 bit 0, +0x2004 (pointer); +0x2074 0x100 on the time-up frame. No other field follows them.
		// +0x2088 byte (struct +0x130): effect flag, set each frame by 0xe0a1b4, cleared by the MS-kind handler
		// (0x2a7560 cases 4/6) of the model update 0x1e4340, run for the own player + players in view only.
		// Own player only: u16 +0xcc set on the own machine, 0 on others;
		// u16 +0x92 follows u16 +0x90 (gauge 4000, equal on all) on the own machine, stays 4000 on others.
		constexpr std::pair<u32, u32> PW_MASK_BITS[] = {{0x58, 0x100}, {0x68, 0x300}, {0x9c, 0x10000}, {0x2004, ~0u}, {0x2068, 1}, {0x2074, 0x100}, {0x2088, 0xff},
			{0xcc, 0xffff}, {0x90, 0xffff0000}};
		std::map<int, std::array<u64, 5>> s_pw;
		// XXH3 of player p's work with the fields above masked (synctest hash=pw).
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
		// Synctest hash=pos: x, y, z (3 floats at player work + 0x2a8) of the 4
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
		// The replay file's per-frame state hash (replay.proto state_hashes): the 4 masked player works (PwHash) and
		// RNG B. RNG A is left out: its machine-local draws would make the files of one battle's positions differ.
		u32 ReplayStateHash()
		{
			u64 h[5];
			for (u32 p = 0; p < 4; p++)
				h[p] = PwHash(p);
			u16 b;
			std::memcpy(&b, &eeMem->Main[RNG_B], 2);
			h[4] = b;
			const u64 x = XXH3_64bits(h, sizeof(h));
			return static_cast<u32>(x ^ (x >> 32));
		}
		// u16 RNG A << 16 | u16 RNG B (replay.proto start_rng, load_rngs)
		u32 GameRng()
		{
			u16 a, b;
			std::memcpy(&a, &eeMem->Main[RNG_A], 2);
			std::memcpy(&b, &eeMem->Main[RNG_B], 2);
			return (static_cast<u32>(a) << 16) | b;
		}
		static constexpr u32 TICK_STATE = 0xc627b4; // u8 game phase; 8 = battle load
		static constexpr u8 TICK_LOAD = 8, TICK_PLAY = 6, TICK_END = 7; // 7 = round or game end phase
		// ZDXSV_PW_DUMP=file: every save appends (s32 frame, 4 * PW_SIZE bytes of player work); rollback
		// re-saves a frame, the last record wins (`tests/zdxsv/pwdiff.py` finds the fields behind H mismatches).
		std::FILE* s_pw_dump = [] {
			const char* p = Zdxsv::TestEnv("ZDXSV_PW_DUMP");
			return p ? std::fopen(p, "wb") : nullptr;
		}();
		// zds: kind 3 (round handshake) is the one barrier: each machine reaches it at its own frame
		// (scene/load timing: one side can be a frame later), so it goes through the GGPO input
		// and the n-th kind 3 of every remote goes to recv once all peers' n-th is in the synced stream.
		std::vector<std::vector<u8>> s_zds_k3[GGPO_MAX_PLAYERS]; // per sender, by index
		u32 s_zds_k3rel = 0;
		// Play start (always on with GGPO, replays included). The battle load step 0x2b1d60 (scene step: waits for the load-busy
		// flag via 0x214260, then inits the per-battle work and sets tick state 8) passes 0x2b1d80 when this
		// machine's load is done: local timing (player work can be initialized 1 frame apart). The rec hook
		// there counts the wish, returns 0 (step retried next frame) until every peer's synced count in
		// Input::unused[1] reaches n, then lets the n-th pass. Rollback state (RollbackState::ps).
		struct PS
		{
			u8 n, rel;
			bool hold, go;
		};
		u32 s_zd_steps = 0, s_zd_changed = 0;
		bool s_net_armed = false, s_net_over = false;
		bool s_lobby_cut = false; // lobby=1 ping test failed: the battle connection is silent (LobbyCutCall)
		u32 s_cut_sends = 0;
		int s_net_me = -1; // local battle position
		std::vector<std::vector<u8>> s_net_sent; // every msg the game sent since armed, in order
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
		int s_net_frame = 0; // GGPO frame being run (last save or load)
		// The HLE state outside the VM that a rollback (s_net_at) or a replay key (PlayKey) restores with the
		// VM state. A new field here is saved and restored everywhere.
		struct RollbackState
		{
			size_t net_pos = 0; // msgs sent so far in the current timeline
			std::vector<u8> net_rx; // remote msgs not yet given to the game's recv
			int zds_seen[GGPO_MAX_PLAYERS] = {}, zds_rel = 0; // zds kind-3 barrier counters
			PS ps = {};
		} s_rb;
		RollbackState s_net_at[128]; // per frame & 127, at its save
		// Per-frame rings that a rollback leaves in place (they hold every frame it can go back to) and a
		// replay key copies whole.
		struct FrameRings
		{
			Input zd_pad[128] = {}; // own host pad per frame & 127 (reruns reapply it)
			u16 zd_hist[128][GGPO_MAX_PLAYERS][2] = {}; // synced (A, B) per frame & 127
			u8 net_seq_at[64][GGPO_MAX_PLAYERS] = {}; // per frame & 63: each player's synced seq
		} s_rings;
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
		bool PlayCommonArm(int pov, int seek = -1);
		void RbkReset();
		void PlayCommonStart();
		void PlayBegin(int me);
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
		std::string s_upload_url; // upload=URL (test), else setting DEV9/Eth ZdxsvReplayUploadUrl
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
				else if (key == "upload")
					s_upload_url = value;
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

		// replay= (net): the lobby answers and HLE state at GGPO frame 0 + the synced inputs of every player per
		// frame; playback starts from the hosted common state (PlayCommonState). File format: see ReplayWrite.
		bool s_replay_rec = false; // a battle is being recorded
		std::string s_replay_rec_dir; // where it is saved, "" = not saved
		std::vector<NetInput> s_replay_inputs; // [frame * s_players + position]
		// per frame, before its inputs: ReplayStateHash, GameRng, the tick state (load ends -> load_frames), the play
		// starts passed (-> play_start_frames, game_end_frames)
		std::vector<u32> s_replay_hashes, s_replay_rngs;
		std::vector<u8> s_replay_ticks, s_replay_ps;
		s64 s_replay_start_at = 0; // unix seconds
		int s_replay_confirmed = -1; // GGPO's last confirmed frame: the frames after it (predicted inputs) are not written
		constexpr int REPLAY_FILE_VERSION = 20261008; // BattleLogFile.log_file_version of the files written here
		// HLE state outside the save state at frame 0 (ReplayBegin; replay.proto net_rx0, hle0)
		struct ReplayHle0
		{
			std::vector<u8> rx;
			std::vector<int64_t> hle;
		} s_replay_hle0;
		std::vector<std::vector<u8>> s_replay_answers; // the battle start's lobby answers (Zdxsv::LobbyStartAnswers) at frame 0
		// The battle's live uplink (battle info live_uplink=, the lobby's UDP address): fed the confirmed frames;
		// left running after Stop until the lobby acked the close, replaced by the next battle's.
		std::shared_ptr<Zdxsv::LiveUp> s_live;

		std::string ReplayIds()
		{
			std::lock_guard lock(s_lobby_mtx);
			return s_lobby ? s_report_ids : std::string();
		}

		std::string IdValue(const std::string& ids, std::string_view key)
		{
			const std::string k = std::string(key) + "=";
			const size_t at = ids.starts_with(k) ? 0 : ids.find("\n" + k);
			if (at == std::string::npos)
				return {};
			const size_t from = at + (at ? 1 : 0) + k.size();
			return ids.substr(from, ids.find('\n', from) - from);
		}

		// The BattleLogFile fields known at frame 0 (the live header; ReplayWrite adds the end, inputs and state).
		std::vector<uint8_t> ReplayHeader(const std::string& code, const std::string& ids)
		{
			using namespace Pb;
			std::vector<uint8_t> pb;
			PutString(pb, 3, code);
			PutInt(pb, 4, REPLAY_FILE_VERSION);
			PutString(pb, 5, "zdxsv-ps2");
			for (size_t p = 0; p < s_net_players.size(); p++)
			{
				std::vector<uint8_t> user;
				PutString(user, 1, s_net_players[p].first);
				PutString(user, 2, s_net_players[p].second);
				PutInt(user, 12, static_cast<int64_t>(p));
				PutBytes(pb, 11, user.data(), user.size());
			}
			PutInt(pb, 20, s_replay_start_at);
			PutInt(pb, 40, s_players);
			PutInt(pb, 41, s_net_me);
			PutInt(pb, 42, s_delay);
			PutString(pb, 43, ids);
			PutBytes(pb, 45, s_replay_hle0.rx.data(), s_replay_hle0.rx.size());
			PutPackedInts(pb, 46, s_replay_hle0.hle);
			PutInt(pb, 47, sizeof(NetInput));
			for (const std::vector<u8>& a : s_replay_answers)
				PutBytes(pb, 51, a.data(), a.size());
			return pb;
		}

		// The confirmed frames not yet given to the live uplink.
		void LiveFeed(size_t frames)
		{
			if (!s_live)
				return;
			frames = std::min(frames, s_replay_inputs.size() / s_players);
			if (const size_t have = s_live->Frames(); frames > have)
				s_live->AddFrames(s_replay_inputs.data() + have * s_players, frames - have);
		}

		std::string ReplayDir()
		{
			if (s_replay_off || !s_net)
				return {};
			if (!s_replay_dir.empty())
				return s_replay_dir;
			return s_lobby ? Path::Combine(EmuFolders::DataRoot, "replays") : std::string();
		}

		std::string ReplayUploadUrl()
		{
			return !s_upload_url.empty() ? s_upload_url : Host::GetStringSettingValue("DEV9/Eth", "ZdxsvReplayUploadUrl", "");
		}

		// At GGPO frame 0: the session started, its first input not added yet (Zdxsv::DeltaStateSave(0) saves this point).
		// Records when the battle is saved, uploaded or streamed live: each one goes without the others (as gdxsv).
		void ReplayBegin()
		{
			if (!s_net)
				return;
			std::string dir = ReplayDir();
			const std::string ids = ReplayIds(), code = IdValue(ids, "battle_code"), to = IdValue(ids, "live_uplink");
			Error error;
			if (!dir.empty() && !FileSystem::DirectoryExists(dir.c_str()) && !FileSystem::CreateDirectoryPath(dir.c_str(), true, &error))
			{
				Console.Error("ZdxsvGgpo: replay: cannot create %s: %s", dir.c_str(), error.GetDescription().c_str());
				dir.clear();
			}
			const bool upload = !code.empty() && !ReplayUploadUrl().empty();
			if (dir.empty() && !upload && to.empty())
				return;
			s_replay_rec = true;
			s_replay_start_at = static_cast<s64>(std::time(nullptr));
			s_replay_rec_dir = dir;
			s_replay_inputs.clear();
			s_replay_hashes.clear();
			s_replay_rngs.clear();
			s_replay_ticks.clear();
			s_replay_ps.clear();
			s_replay_confirmed = -1;
			// HLE state outside the save state at frame 0 (PlayLoad restores it): msgs waiting for the game's recv,
			// play-start barrier, kind-3 barrier
			s_replay_hle0.rx.assign(s_rb.net_rx.begin(), s_rb.net_rx.end());
			s_replay_hle0.hle = {s_rb.ps.n, s_rb.ps.rel, s_rb.ps.hold ? 1 : 0, s_rb.ps.go ? 1 : 0, s_rb.zds_seen[0], s_rb.zds_seen[1],
				s_rb.zds_seen[2], s_rb.zds_seen[3], s_rb.zds_rel};
			s_replay_answers = Zdxsv::LobbyStartAnswers();
			s_live.reset();
			if (!to.empty())
				s_live = std::make_shared<Zdxsv::LiveUp>(to, code, s_lobby_session, ReplayHeader(code, ids), s_players * sizeof(NetInput));
			Console.WriteLn("ZdxsvGgpo: replay: recording (save %s, upload %d, live %d)", dir.empty() ? "off" : dir.c_str(), upload ? 1 : 0,
				to.empty() ? 0 : 1);
		}

		// The synced inputs of frame f (also rerun frames: a rollback replaces the frames from f on).
		void ReplayLog(int f, const NetInput* in)
		{
			if (!s_replay_rec || f < 0)
				return;
			s_replay_inputs.resize(static_cast<size_t>(f) * s_players);
			s_replay_inputs.insert(s_replay_inputs.end(), in, in + s_players);
			s_replay_hashes.resize(f);
			s_replay_hashes.push_back(ReplayStateHash());
			s_replay_rngs.resize(f);
			s_replay_rngs.push_back(GameRng());
			s_replay_ticks.resize(f);
			s_replay_ticks.push_back(eeMem->Main[TICK_STATE]);
			s_replay_ps.resize(f);
			s_replay_ps.push_back(s_rb.ps.rel);
			int confirmed = -1;
			if (s_session && ggpo_get_last_confirmed_frame(s_session, &confirmed) == GGPO_OK)
				s_replay_confirmed = std::max(s_replay_confirmed, confirmed);
			LiveFeed(static_cast<size_t>(s_replay_confirmed + 1));
		}

		// As flycast's GdxsvBackendRollback::SaveReplay: every player posts <battle_code>.pb (multipart field "file")
		// to the uploader, which keeps the first one (409 for the others). Detached: a slow upload blocks neither
		// the next battle nor the exit.
		void ReplayUpload(const std::string& name, const std::vector<uint8_t>& pb)
		{
			const std::string url = ReplayUploadUrl();
			if (url.empty())
				return;
			const std::string boundary = fmt::format("zdxsv{:016x}", XXH64(pb.data(), pb.size(), 0));
			std::string body = fmt::format("--{}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{}.pb\"\r\n"
										   "Content-Type: application/octet-stream\r\n\r\n",
				boundary, name);
			body.append(reinterpret_cast<const char*>(pb.data()), pb.size());
			body += fmt::format("\r\n--{}--\r\n", boundary);
			std::thread([url, body = std::move(body), type = "multipart/form-data; boundary=" + boundary]() mutable {
				std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create(Host::GetHTTPUserAgent());
				if (!http)
				{
					Console.Error("ZdxsvGgpo: replay upload: no HTTP client");
					return;
				}
				http->SetTimeout(300.0f);
				Common::Timer timer;
				const size_t size = body.size();
				http->CreatePostRequest(url, std::move(body), [&](s32 status, const std::string&, HTTPDownloader::Request::Data) {
					if (status == HTTPDownloader::HTTP_STATUS_OK || status == 409)
						Console.WriteLn("ZdxsvGgpo: replay upload %s: %s, %zu bytes, %.0f ms", url.c_str(),
							status == 409 ? "already there" : "ok", size, timer.GetTimeMilliseconds());
					else
						Console.Error("ZdxsvGgpo: replay upload %s failed: status %d", url.c_str(), status);
				}, nullptr, std::move(type));
				http->WaitForAllRequests();
			}).detach();
		}

		// replay.proto play_start_frames / game_end_frames of the first `frames` frames: a play start = the frame the
		// play-start barrier passes; a game end = the last tick state 6 -> 7 before the next play start (or the file end).
		// The peers enter the game end on different frames (the end phase waits on the HLE'd battle socket), so sync
		// checks stop there. Earlier 6 -> 7 after the same play start are round ends (replay.proto round_end_frames):
		// pairs (6 -> 7, the tick's next 6), the peers enter the load between them on different frames.
		void ReplayGameEnds(size_t frames, std::vector<int64_t>& starts, std::vector<int64_t>& ends, std::vector<int64_t>& rounds)
		{
			frames = std::min({frames, s_replay_ticks.size(), s_replay_ps.size()});
			int64_t end = -1, round_to = -1;
			for (size_t f = 1; f < frames; f++)
			{
				if (s_replay_ps[f] != s_replay_ps[f - 1])
				{
					if (end >= 0)
						ends.push_back(end);
					end = -1;
					starts.push_back(static_cast<int64_t>(f - 1)); // passed while applying frame f - 1's inputs
				}
				// f - 1: the frame whose step enters 7 (the trace's `L` frame); the peers' state hashes differ from there
				if (!starts.empty() && s_replay_ticks[f - 1] == TICK_PLAY && s_replay_ticks[f] == TICK_END)
				{
					if (end >= 0 && round_to >= 0)
					{
						rounds.push_back(end);
						rounds.push_back(round_to);
					}
					end = static_cast<int64_t>(f - 1);
					round_to = -1;
				}
				else if (end >= 0 && round_to < 0 && s_replay_ticks[f - 1] != TICK_PLAY && s_replay_ticks[f] == TICK_PLAY)
					round_to = static_cast<int64_t>(f - 1);
			}
			if (end >= 0)
				ends.push_back(end);
		}

		// <dir>/<battle_code>.pb (without a battle code: rbk-<start_at>-p<position>.pb): a BattleLogFile protobuf,
		// schema in Zdxsv/replay.proto.
		void ReplayWrite(const char* what)
		{
			if (!std::exchange(s_replay_rec, false))
				return;
			Common::Timer timer;
			const std::string dir = std::exchange(s_replay_rec_dir, {});
			const size_t frames = std::min<size_t>(s_replay_inputs.size() / s_players, s_replay_confirmed + 1);
			if (s_live)
			{
				LiveFeed(frames);
				s_live->Close(what);
			}
			const std::string ids = ReplayIds();
			const std::string code = IdValue(ids, "battle_code");
			std::string name;
			for (const char c : code)
				name += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
			if (name.empty())
				name = fmt::format("rbk-{}-p{}", s_replay_start_at, s_net_me);
			using namespace Pb;
			std::vector<uint8_t> pb = ReplayHeader(code, ids);
			pb.reserve(frames * s_players * sizeof(NetInput) + pb.size() + 1024);
			PutInt(pb, 21, static_cast<s64>(std::time(nullptr)));
			PutString(pb, 24, what);
			PutInt(pb, 48, static_cast<int64_t>(frames));
			PutBytes(pb, 49, s_replay_inputs.data(), frames * s_players * sizeof(NetInput));
			PutBytes(pb, 52, s_replay_hashes.data(), frames * sizeof(u32));
			if (frames > 0)
				PutUint(pb, 53, s_replay_rngs[0]);
			std::vector<int64_t> load_frames, load_rngs;
			for (size_t f = 1; f < frames; f++)
			{
				if (s_replay_ticks[f - 1] == TICK_LOAD && s_replay_ticks[f] != TICK_LOAD)
				{
					load_frames.push_back(static_cast<int64_t>(f));
					load_rngs.push_back(s_replay_rngs[f]);
				}
			}
			PutPackedInts(pb, 54, load_frames);
			PutPackedInts(pb, 55, load_rngs);
			std::vector<int64_t> starts, ends, rounds;
			ReplayGameEnds(frames, starts, ends, rounds);
			PutPackedInts(pb, 57, starts);
			PutPackedInts(pb, 58, ends);
			PutPackedInts(pb, 59, rounds);
			if (!dir.empty())
			{
				const std::string path = Path::Combine(dir, name + ".pb");
				if (!FileSystem::WriteBinaryFile(path.c_str(), pb.data(), pb.size()))
					Console.Error("ZdxsvGgpo: replay: write %s failed", path.c_str());
				else
					Console.WriteLn("ZdxsvGgpo: replay saved %s frames=%zu, file=%zu bytes, %.1f ms", path.c_str(), frames,
						pb.size(), timer.GetTimeMilliseconds());
			}
			if (!code.empty())
				ReplayUpload(name, pb);
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
			if (s_rbk && !s_play_env && !s_vm_closing)
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
			s_rb = {};
			s_net_out.clear();
			s_net_local = {};
			s_net_frame = 0;
			std::fill(std::begin(s_net_at), std::end(s_net_at), RollbackState{});
			s_net_end = -1;
			s_ns = {};
			s_rings = {};
			s_zd_steps = s_zd_changed = s_zds_echo = s_zds_skip = s_zds_k3rel = 0;
			for (auto& k3 : s_zds_k3)
				k3.clear();
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
		if (s_rbk && !s_play_env && !rbk_env_logged)
		{
			rbk_env_logged = true;
			const auto env = [](const char* k) { const char* v = std::getenv(k); return v && *v ? v : "-"; };
			Console.WriteLn("ZdxsvGgpo: rbk env pos=%d/%d rand=%s turbo=%s clamp=%s ggpo=%s", s_rbk_me, s_rbk_n,
				env("ZDXSV_RAND_INPUT"), env("ZDXSV_RBK_TURBO"), env("ZDXSV_EE_CLAMP"), env("ZDXSV_GGPO"));
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
			if (s_play_env && !(s_play_common && s_net_armed)) // PlayLoad starts it, or the arm of a common start
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
			s_play_common ? PlayCommonStart() : PlayNext();
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
	bool g_ps_hook = true;

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
		if (!g_z_game) // any other game or build: no GGPO, replay, hooks or platform info
		{
			s_options.clear();
			g_ggpo_enabled = s_net_env = g_net_hook = g_zd_hook = g_ps_hook = false;
			if (serial_match || (e && std::strcmp(e, "0") != 0) || s_play_env || s_net_trace)
				Console.Warning("ZdxsvGgpo: off: not the Z game (serial %s CRC %08X, need %s %08X)", serial, crc, GAME_SERIAL, want);
			return;
		}
		if (e && std::strcmp(e, "0") == 0) // off whatever the setting (rigs without GGPO)
			s_options.clear();
		else
			s_options = e ? e : lobby_default ? DEFAULT_OPTIONS : "";
		g_ggpo_enabled = !s_options.empty() || s_play_env;
		s_net_env = s_options.find("net=1") != std::string::npos || s_play_env;
		g_net_hook = s_net_trace != nullptr || s_net_env;
		g_zd_hook = s_net_env;
		g_ps_hook = true;
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

	// The game's input record (A, B) of a host pad: its button bind table (OR-linear; B bit 0 = game state, left 0).
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
		if (!s_rb.ps.hold)
		{
			s_rb.ps.hold = true;
			s_rb.ps.n++;
			if (s_net_trace)
				std::fprintf(s_net_trace, "%u PH%s %d %d\n", g_FrameCount, g_ggpo_in_rollback ? "r" : "", s_net_frame, s_rb.ps.n);
		}
		if (s_rb.ps.go)
		{
			s_rb.ps.hold = s_rb.ps.go = false;
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
		const u16* ab = s_rings.zd_hist[k & 127][p];
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
		// `A vsync frame A0..A3`: pad module A cur per position (0x6f2500 + 16p, the applied input)
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
	// reader 0x2ba63c), bytes 0-7 of each + the base, when changed.
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
		// entry; 1800 and 0xffff in earlier phases; tools/zdxsv/ramcount.py).
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
						s_rb.net_rx.push_back(m[0]);
						s_rb.net_rx.push_back(static_cast<u8>((m[1] & 0xf0) | q));
						s_rb.net_rx.insert(s_rb.net_rx.end(), m.begin() + 2, m.end());
					}
				if (!g_ggpo_in_rollback)
					s_zds_echo++;
			}
			else if (!g_ggpo_in_rollback && s_zds_skip++ < 20)
				Console.Warning("ZdxsvGgpo: zds msg kind %d (%zu bytes) not echoed", kind, m.size());
			if (s_rb.net_pos < s_net_sent.size())
			{
				if (s_net_sent[s_rb.net_pos] != m && s_ns.senddiff++ < 40)
				{
					std::string a, b;
					char h[4];
					for (u8 v : s_net_sent[s_rb.net_pos])
						std::snprintf(h, sizeof(h), "%02x", v), a += h;
					for (u8 v : m)
						std::snprintf(h, sizeof(h), "%02x", v), b += h;
					Console.Warning("ZdxsvGgpo: net rerun send %zu differs (frame %d) sent %s rerun %s", s_rb.net_pos, s_net_frame, a.c_str(), b.c_str());
				}
				if (s_net_sent_at[s_rb.net_pos] != s_net_frame && m.size() >= 2 && (m[1] >> 4) == 3)
				{
					auto it = std::find_if(s_net_out.begin(), s_net_out.end(), [](const NetOut& o) { return o.idx == s_rb.net_pos; });
					if (it != s_net_out.end())
						it->frame = s_net_frame, s_ns.restamp++;
					else if (s_ns.late++ < 20)
						Console.Warning("ZdxsvGgpo: net rerun send %zu moved %d -> %d after it went into an input", s_rb.net_pos, s_net_sent_at[s_rb.net_pos], s_net_frame);
					if (s_net_trace)
						std::fprintf(s_net_trace, "%u OM %zu %d %d\n", g_FrameCount, s_rb.net_pos, s_net_sent_at[s_rb.net_pos], s_net_frame);
				}
				s_net_sent_at[s_rb.net_pos] = s_net_frame;
			}
			else
			{
				if (m.size() >= 2 && (m[1] >> 4) == 0xf && s_net_end < 0)
					s_net_end = 0;
				s_net_sent.push_back(m);
				s_net_sent_at.push_back(s_net_frame);
				if (m.size() >= 2 && (m[1] >> 4) == 3)
					s_net_out.push_back({s_net_frame, s_rb.net_pos, std::move(m)});
				s_ns.maxq = std::max<u32>(s_ns.maxq, static_cast<u32>(s_net_out.size()));
			}
			s_rb.net_pos++;
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
			s_net_at[frame & 127] = s_rb;
			if (s_pw_hash)
			{
				std::array<u64, 5>& h = s_pw[frame];
				for (u32 p = 0; p < 4; p++)
					h[p] = XXH3_64bits(&eeMem->Main[PW_BASE + PW_SIZE * p + PW_POS], 12);
				u16 a, b;
				std::memcpy(&a, &eeMem->Main[RNG_A], 2);
				std::memcpy(&b, &eeMem->Main[RNG_B], 2);
				h[4] = (static_cast<u64>(a) << 16) | b;
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
			s_rb = s_net_at[frame & 127];
		}

		bool NetStart(GGPOSessionCallbacks& cb)
		{
			if (s_net_me < 0 || s_net_me >= s_players)
			{
				Console.Error("ZdxsvGgpo: net position %d not below players=%d", s_net_me, s_players);
				return false;
			}
			const int local_port = s_lobby ? s_port : s_port + s_net_me;
			if (s_net_trace) // battle start marker (pwcheck.py --battle): BATTLES=N lobby battles in one process
			{
				const std::string code = IdValue(ReplayIds(), "battle_code");
				std::fprintf(s_net_trace, "%u B %s\n", g_FrameCount, code.empty() ? "-" : code.c_str());
			}
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

		// Own input of frame s_net_frame (live: GGPO adds the delay; replay takeover: PlayFrame): pad 0 undelayed to
		// the game, its (A, B) + the own kind-3 msgs due since the last input.
		NetInput NetPack(const Input& pad)
		{
			NetInput in = s_net_local;
			in.pad = pad;
			s_rings.zd_pad[s_net_frame & 127] = in.pad;
			u16 ab[2];
			ZdPadAB(in.pad, ab[0], ab[1]);
			if (s_net_trace)
				std::fprintf(s_net_trace, "%u Q %d %04x %04x\n", g_FrameCount, s_net_frame, ab[0], ab[1]);
			in.pad = {};
			std::memcpy(&in.pad, ab, sizeof(ab));
			in.pad.unused[1] = s_rb.ps.n;
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
			return in;
		}

		bool NetNextInputs()
		{
			NetInput in = NetPack(s_rand_env ? RandInput() : HostInput());
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

		// Inputs of frame s_net_frame: own host pad to pad 0, every position's (A, B) to s_rings.zd_hist, kind-3
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
			ApplyPad(0, s_rings.zd_pad[f & 127]);
			for (int p = 0; p < s_players; p++)
				std::memcpy(s_rings.zd_hist[f & 127][p], &in[p].pad, sizeof(s_rings.zd_hist[f & 127][p]));
			// a predicted input repeats its seq, so entries come only from real inputs: no rollback state
			for (int p = 0; p < s_players; p++)
			{
				const bool fresh = in[p].seq != s_rings.net_seq_at[(f - 1) & 63][p];
				const u8* d = in[p].data;
				const u32 len = std::min<u32>(in[p].len, sizeof(in[p].data));
				if (fresh)
					for (u32 i = 0; i + 1 < len && d[i] >= 2 && i + d[i] <= len; i += d[i])
						if ((d[i + 1] >> 4) == 3)
						{
							std::vector<std::vector<u8>>& v = s_zds_k3[p];
							if (v.size() <= static_cast<size_t>(s_rb.zds_seen[p]))
								v.resize(s_rb.zds_seen[p] + 1);
							v[s_rb.zds_seen[p]++].assign(d + i, d + i + d[i]);
						}
				s_rings.net_seq_at[f & 63][p] = in[p].seq;
			}
			for (;;)
			{
				bool all = true;
				for (int p = 0; p < s_players; p++)
					all = all && s_rb.zds_seen[p] > s_rb.zds_rel;
				if (!all)
					break;
				for (int p = 0; p < s_players; p++)
					if (p != s_net_me)
						s_rb.net_rx.insert(s_rb.net_rx.end(), s_zds_k3[p][s_rb.zds_rel].begin(), s_zds_k3[p][s_rb.zds_rel].end());
				if (s_net_trace)
					std::fprintf(s_net_trace, "%u K3%s %d %d\n", g_FrameCount, g_ggpo_in_rollback ? "r" : "", f, s_rb.zds_rel);
				if (!g_ggpo_in_rollback)
					s_zds_k3rel++;
				s_rb.zds_rel++;
			}
			if (s_rb.ps.hold && !s_rb.ps.go)
			{
				bool all = true;
				for (int p = 0; p < s_players; p++)
					all = all && static_cast<u8>(in[p].pad.unused[1] - s_rb.ps.rel) >= 1 && static_cast<u8>(in[p].pad.unused[1] - s_rb.ps.rel) < 128;
				if (all)
				{
					s_rb.ps.go = true;
					s_rb.ps.rel++;
					if (s_net_trace)
						std::fprintf(s_net_trace, "%u PS%s %d %d\n", g_FrameCount, g_ggpo_in_rollback ? "r" : "", f, s_rb.ps.rel);
				}
			}
		}

		void NetReport()
		{
			Console.WriteLn("ZdxsvGgpo: net sends %u msgs %u (sent %zu, unsent %zu) recvs %u rxmsgs %u rxbytes %u polls %u other %u nowait %u senddiff %u toolong %u maxq %u waits %d k3lag %d restamp %u late %u",
				s_ns.sends, s_ns.msgs, s_net_sent.size(), s_net_out.size(), s_ns.recvs, s_ns.rxmsgs, s_ns.rxbytes, s_ns.polls,
				s_ns.other, s_ns.nowait, s_ns.senddiff, s_ns.toolong, s_ns.maxq, s_waits, s_k3_lag, s_ns.restamp, s_ns.late);
			Console.WriteLn("ZdxsvGgpo: zd steps %u changed %u echo %u skip %u k3 %d/%d/%d/%d rel %d (fwd %u)", s_zd_steps, s_zd_changed, s_zds_echo, s_zds_skip,
				s_rb.zds_seen[0], s_rb.zds_seen[1], s_rb.zds_seen[2], s_rb.zds_seen[3], s_rb.zds_rel, s_zds_k3rel);
			if (s_pw_hash && s_net_trace)
			{
				for (const auto& [f, h] : s_pw)
					std::fprintf(s_net_trace, "0 H %d %016llx %016llx %016llx %016llx %08llx\n", f, static_cast<unsigned long long>(h[0]),
						static_cast<unsigned long long>(h[1]), static_cast<unsigned long long>(h[2]), static_cast<unsigned long long>(h[3]),
						static_cast<unsigned long long>(h[4]));
				std::fflush(s_net_trace);
				Console.WriteLn("ZdxsvGgpo: pw hashes %zu frames", s_pw.size());
				s_pw.clear();
			}
			if (s_pw_dump)
				std::fflush(s_pw_dump);
		}

		// ZDXSV_REPLAY (s_play_env): the file's frame 0 state is loaded, the HLE state of frame 0 restored
		// (net_rx0 / hle0), then each frame gets the file's synced inputs (NetApply, as a live frame without
		// rollback): the battle sock HLE, zd step copy and both barriers run as live. Own pad 0 = the own
		// position's B bits mapped back to buttons (no sticks in the file). ZDXSV_REPLAY_EXIT=1: exit at the
		// end (else pause); ZDXSV_REPLAY_TURBO=1: turbo limiter. With ZDXSV_PW_HASH + ZDXSV_NET_TRACE the H lines
		// compare to the live battle's (zdxsv/pwcheck.py).
		std::vector<NetInput> s_play_inputs;
		// The file's state checks (replay.proto 52..55, optional): ReplayStateHash per frame, GameRng at frame 0 and at
		// each load end, of position s_play_rng_pos (RNG A is per machine). Checked before a frame's inputs (PlayFrame).
		std::vector<u32> s_play_hashes;
		std::map<int, u32> s_play_rngs; // frame -> GameRng
		int s_play_rng_pos = -1;
		int s_play_hash_checks = 0, s_play_hash_bad = 0, s_play_hash_first_bad = -1, s_play_rng_checks = 0, s_play_rng_bad = 0;
		// [game end, next play start) and [round end, tick 6) of the file (replay.proto 57..59): not checked, the peers differ there
		std::vector<std::pair<int, int>> s_play_cut;
		int s_play_hash_cut = 0;
		int s_play_frames = 0;
		std::vector<std::vector<u8>> s_play_answers; // common start: the file's lobby answers (RbkBody)
		int s_play_rearm_seek = -1; // a switch's common start: the frame to seek to from its key 0
		LimiterModeType s_play_rearm_limiter = LimiterModeType::Nominal; // and the limiter before it
		bool s_play_booted_saved = false; // ZDXSV_REPLAY_STATE empty: the booted state is in the cache (PlayCommonState)

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
			RollbackState rb;
			FrameRings rings;
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
		// The state before frame f runs, zipped to `name` in the cache folder on a thread.
		std::unique_ptr<PlayKey> PlayKeyMake(int f, const std::string& name)
		{
			Error error;
			std::unique_ptr<ArchiveEntryList> list = SaveState_DownloadState(&error);
			if (!list)
			{
				Console.Error("ZdxsvGgpo: replay key %d: state download failed: %s", f, error.GetDescription().c_str());
				return nullptr;
			}
			auto key = std::make_unique<PlayKey>();
			PlayKey* k = key.get();
			k->path = Path::Combine(EmuFolders::Cache, name);
			k->rb = s_rb;
			k->rings = s_rings;
			k->zip = std::thread([list = std::move(list), k]() mutable {
				Error error;
				k->ok = SaveState_ZipToDisk(std::move(list), nullptr, k->path.c_str(), &error);
				if (!k->ok)
					Console.Error("ZdxsvGgpo: replay key: state zip failed: %s", error.GetDescription().c_str());
			});
			return key;
		}

		void PlayKeySave(int f)
		{
			if (s_play_key_every <= 0 || f % s_play_key_every != 0 || s_play_keys[s_net_me].contains(f))
				return;
			Common::Timer timer;
			std::unique_ptr<PlayKey> key = PlayKeyMake(f, fmt::format("zdxsv-replay-key-p{}-{}.p2s", s_net_me, f));
			if (!key)
				return;
			s_play_keys[s_net_me].emplace(f, std::move(key));
			Console.WriteLn("ZdxsvGgpo: replay key %d (download %.1f ms)", f, timer.GetTimeMilliseconds());
		}

		void PlayKeyApply(const PlayKey& k)
		{
			s_rb = k.rb;
			s_rings = k.rings;
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

		// Takeover (as flycast's GdxsvBackendReplay): from frame T the own position plays the host pad, packed as in a
		// live battle (NetPack) with an input delay of mindelay= frames; the other positions keep the file's inputs
		// (past its end: no buttons, no new msgs). The first `delay` own inputs are the file's (added before T). It
		// starts paused at T: the host pad must hold the replay's own input at T, then keep it for 1 s (START skips
		// the matching). START while taken over = retry from T. Keys and battle loads stay the replay's.
		// ZDXSV_REPLAY_TAKEOVER=frame[:replay|:rand] (tests): take over at `frame` with no matching; `replay` = the host
		// pad is the file's own input `delay` frames ahead (plays the replay unchanged), `rand` = random buttons.
		// ZDXSV_REPLAY_TAKEOVER_RETRY=frame: retry at that frame.
		enum : int { TO_OFF, TO_ALIGN, TO_COUNT, TO_ON };
		enum : int { TO_REQ_NONE, TO_REQ_TAKE, TO_REQ_START, TO_REQ_RETRY, TO_REQ_RETURN, TO_REQ_CANCEL };
		std::atomic<int> s_to_phase{TO_OFF};
		std::atomic<int> s_to_req{TO_REQ_NONE};
		std::atomic<u16> s_to_target{0}; // own B of the file at T
		int s_to_frame = -1; // T
		int s_to_delay = 0;
		std::unique_ptr<PlayKey> s_to_key; // the state before T
		std::vector<std::vector<u8>> s_to_sent; // own sends before T
		std::vector<int> s_to_sent_at;
		std::deque<NetOut> s_to_out; // own kind-3 msgs before T not in the file's own inputs up to T + delay - 1
		NetInput s_to_local; // the file's own input at T + delay - 1
		std::deque<NetInput> s_to_queue; // packed own inputs, `delay` frames ahead
		bool s_to_skip = false, s_to_start_held = false;
		Common::Timer::Value s_to_count_t0 = 0;
		const char* const s_to_test = std::getenv("ZDXSV_REPLAY_TAKEOVER");
		int s_to_test_at = s_to_test ? std::atoi(s_to_test) : -1;
		const char s_to_test_src = [] {
			const char* c = s_to_test ? std::strchr(s_to_test, ':') : nullptr;
			const std::string_view src = c ? c + 1 : "";
			return src == "replay" ? 'r' : src == "rand" ? 'n' : '\0';
		}();
		int s_to_test_retry = [] {
			const char* e = std::getenv("ZDXSV_REPLAY_TAKEOVER_RETRY");
			return e ? std::atoi(e) : -1;
		}();
		std::mt19937 s_to_rng{1};

		const NetInput& PlayRow(int f, int p) { return s_play_inputs[static_cast<size_t>(f) * s_players + p]; }

		u16 PlayOwnB(int f)
		{
			if (f < 0 || f >= s_play_frames)
				return 0;
			u16 ab[2];
			std::memcpy(ab, &PlayRow(f, s_net_me).pad, sizeof(ab));
			return ab[1];
		}

		Input TakeoverPad(int f)
		{
			if (s_to_test_src == 'r')
				return PadFromB(PlayOwnB(f + s_to_delay));
			if (s_to_test_src == 'n')
			{
				static Input in = PadFromB(0);
				if (f % 5 == 0)
				{
					using I = PadDualshock2::Inputs;
					in.buttons = static_cast<u16>(s_to_rng() & s_to_rng() & ~((1u << I::PAD_START) | (1u << I::PAD_SELECT)));
				}
				return in;
			}
			return HostInput();
		}

		// At T = the next frame (the running state = before T): keeps the state and the own sent msgs.
		bool TakeoverBegin(int t)
		{
			s_to_delay = s_min_delay;
			if (t < 1 || t + s_to_delay > s_play_frames)
			{
				Console.Error("ZdxsvGgpo: replay takeover at frame %d: not within the replay (%d frames)", t, s_play_frames);
				return false;
			}
			s_to_key = PlayKeyMake(t, fmt::format("zdxsv-replay-takeover-p{}.p2s", s_net_me));
			if (!s_to_key)
				return false;
			const size_t pos = std::min(s_rb.net_pos, s_net_sent.size());
			s_to_sent.assign(s_net_sent.begin(), s_net_sent.begin() + pos);
			s_to_sent_at.assign(s_net_sent_at.begin(), s_net_sent_at.begin() + pos);
			// the own msg bytes of the file's inputs up to T + delay - 1 = the oldest kind-3 sends, in order
			size_t bytes = 0;
			u8 seq = 0;
			for (int f = 0; f < t + s_to_delay; f++)
			{
				const NetInput& in = PlayRow(f, s_net_me);
				if (in.seq != seq)
					bytes += in.len;
				seq = in.seq;
			}
			s_to_out.clear();
			for (size_t i = 0; i < pos; i++)
			{
				const std::vector<u8>& m = s_to_sent[i];
				if (m.size() < 2 || (m[1] >> 4) != 3)
					continue;
				if (s_to_out.empty() && (m.size() > sizeof(NetInput::data) || m.size() <= bytes))
				{
					if (m.size() <= sizeof(NetInput::data))
						bytes -= m.size();
					continue;
				}
				s_to_out.push_back({s_to_sent_at[i], i, m});
			}
			s_to_local = PlayRow(t + s_to_delay - 1, s_net_me);
			s_to_frame = t;
			s_to_target = PlayOwnB(t);
			Console.WriteLn("ZdxsvGgpo: replay takeover at frame %d, delay %d, own sends %zu, kind-3 not in an input %zu%s", t,
				s_to_delay, pos, s_to_out.size(), bytes ? " (msg bytes of the file left over)" : "");
			return true;
		}

		// Back to the state before T with the own sent msgs of then. Returns T, or -1.
		int TakeoverLoad()
		{
			PlayKey& k = *s_to_key;
			if (k.zip.joinable())
				k.zip.join();
			Error error;
			if (!k.ok || !VMManager::LoadState(k.path.c_str(), &error))
			{
				Console.Error("ZdxsvGgpo: replay takeover: state load failed: %s", error.GetDescription().c_str());
				return -1;
			}
			PlayKeyApply(k);
			s_net_sent = s_to_sent;
			s_net_sent_at = s_to_sent_at;
			s_net_out.clear();
			s_to_start_held = true; // a START held now retries on its release + press only
			return s_to_frame;
		}

		void TakeoverStart()
		{
			s_net_sent = s_to_sent;
			s_net_sent_at = s_to_sent_at;
			s_net_out = s_to_out;
			s_net_local = s_to_local;
			s_to_queue.clear();
			for (int i = 0; i < s_to_delay; i++)
				s_to_queue.push_back(PlayRow(s_to_frame + i, s_net_me));
			s_to_phase = TO_ON;
			Console.WriteLn("ZdxsvGgpo: replay takeover starts at frame %d, vsync %u", s_to_frame, g_FrameCount);
		}

		void TakeoverOff(const char* why)
		{
			s_to_phase = TO_OFF;
			s_to_queue.clear();
			s_to_key.reset();
			Console.WriteLn("ZdxsvGgpo: replay takeover %s at frame %d, vsync %u", why, s_net_frame, g_FrameCount);
		}

		void PlayFrameTakeover(int f)
		{
			s_net_frame = f;
			PlayBarPublish();
			NetSaved(f);
			NetInput in[GGPO_MAX_PLAYERS];
			std::memcpy(in, &PlayRow(std::min(f, s_play_frames - 1), 0), sizeof(NetInput) * s_players);
			if (f >= s_play_frames)
				for (int p = 0; p < s_players; p++)
					std::memset(&in[p].pad, 0, sizeof(u16) * 2); // (A, B); seq unchanged = no new msgs
			s_to_queue.push_back(NetPack(TakeoverPad(f)));
			in[s_net_me] = s_to_queue.front();
			s_to_queue.pop_front();
			NetApply(in);
		}

		// The file's state hash and RNGs of frame f against the emulated state (a desync check; nothing changes).
		void PlayCheckState(int f)
		{
			const bool cut = std::any_of(s_play_cut.begin(), s_play_cut.end(), [f](const auto& c) { return f >= c.first && f < c.second; });
			if (cut && f < static_cast<int>(s_play_hashes.size()))
				s_play_hash_cut++;
			else if (f < static_cast<int>(s_play_hashes.size()))
			{
				s_play_hash_checks++;
				if (const u32 h = ReplayStateHash(); h != s_play_hashes[f])
				{
					if (s_play_hash_bad++ < 10)
						Console.Warning("ZdxsvGgpo: replay state hash differs at frame %d: file %08x, here %08x", f, s_play_hashes[f], h);
					if (s_play_hash_first_bad < 0)
						s_play_hash_first_bad = f;
				}
			}
			if (const auto it = s_play_rngs.find(f); !cut && it != s_play_rngs.end() && s_net_me == s_play_rng_pos)
			{
				// judged on RNG B (low half): RNG A also differs from the recorder's under the common start
				s_play_rng_checks++;
				const u32 rng = GameRng();
				const bool bad = static_cast<u16>(rng) != static_cast<u16>(it->second);
				if (bad)
					s_play_rng_bad++;
				Console.WriteLn("ZdxsvGgpo: replay rng at frame %d: file %08x, here %08x%s", f, it->second, rng, bad ? " DIFFERS" : "");
			}
		}

		void PlayFrame(int f)
		{
			if (s_to_phase == TO_ON)
			{
				PlayFrameTakeover(f);
				return;
			}
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
			s_rings.zd_pad[f & 127] = PadFromB(ab[1]);
			PlayCheckState(f);
			NetApply(in);
		}

		void PlayReport(const char* what)
		{
			Console.WriteLn("ZdxsvGgpo: replay %s at frame %d of %d, vsync %u", what, s_net_frame, s_play_frames, g_FrameCount);
			Console.WriteLn("ZdxsvGgpo: replay state check: hashes %d checked, %d differ (first at frame %d); rngs %d checked, %d differ; "
							"%d frames after a round or game end not checked",
				s_play_hash_checks, s_play_hash_bad, s_play_hash_first_bad, s_play_rng_checks, s_play_rng_bad, s_play_hash_cut);
			NetReport();
		}

		void PlayStop(const char* what)
		{
			g_ggpo_active = false;
			s_bar_frame = -1;
			s_net_over = true; // the battle sock goes back to the IOP
			PlayReport(what);
			if (const char* e = std::getenv("ZDXSV_REPLAY_EXIT"); e && e[0] == '1')
				Host::RunOnCPUThread([] { Host::RequestVMShutdown(false, false, false); });
			else if (!s_play_at_end)
				Host::RunOnCPUThread([] { VMManager::SetPaused(true); });
		}

		// Point of view: the file holds every position's inputs. ZDXSV_REPLAY_POV=p (default the recorder's) is the position
		// the lobby answers 0x6912 with before the battle start (PlayCommonArm). A switch at frame f loads the new
		// position's newest key <= f and runs to f unlimited (PlaySeek pov_switch); a position never played runs its own
		// battle start first, its key 0 (PlaySwitch). Keys and sent msgs are kept per position.
		std::atomic<int> s_play_pov_req{-1}; // requested position, -1 = none
		std::deque<std::pair<int, int>> s_play_pov_at; // ZDXSV_REPLAY_POV_AT=frame:position,...
		struct PlaySent
		{
			std::vector<std::vector<u8>> sent;
			std::vector<int> at;
			std::deque<NetOut> out;
		};
		PlaySent s_play_sent[GGPO_MAX_PLAYERS];

		// Live spectating: ZDXSV_REPLAY=udp://host:port[/battle code] (no code: the newest live battle there), the lobby's
		// relay of the battle's uplink (Lobby.h LiveDown). PlayLoadFile opens it, PlayNext takes the frames in (LiveNext)
		// and waits at the newest frame, as a GGPO frame waits for a peer, until LIVE_BUFFER more are there or the stream
		// is closed. More than LIVE_BUFFER + LIVE_CATCHUP frames behind (a late join) it runs unlimited until
		// LIVE_BUFFER + LIVE_EDGE (flycast's gdxsv:LiveBufferFrames and its catch-up edges).
		constexpr int LIVE_BUFFER = 30, LIVE_CATCHUP = 270, LIVE_EDGE = 60, LIVE_STALL_MS = 30000, LIVE_OPEN_MS = 15000;
		std::unique_ptr<Zdxsv::LiveDown> s_live_down;
		Zdxsv::LiveStreams s_live_got; // inputs not in s_play_inputs yet (part of a frame)
		std::string s_live_close; // "" = running
		bool s_live_catchup = false, s_live_close_logged = false;
		LimiterModeType s_live_limiter = LimiterModeType::Nominal;
		int s_live_waits = 0;
		double s_live_wait_ms = 0;

		// Auto-next (flycast's gdxsv:LiveAutoNext; setting ZdxsvLiveAutoNext, ZDXSV_LIVE_NEXT=N: N more battles, 0 = off):
		// at the end of a closed stream a thread asks the lobby every LIVE_NEXT_POLL_S for the newest running battle not
		// watched yet (LiveDown::Newest with the last LIVE_NEXT_SKIP watched; gdxsv's live_autoplay_pick), which resets the VM,
		// and PlayLoad opens it instead of ZDXSV_REPLAY. A battle moved on to counts as watched even if it fails to open.
		struct LiveWait
		{
			std::atomic<bool> quit{false};
			std::thread t;
			~LiveWait()
			{
				quit = true;
				if (t.joinable())
					t.join();
			}
		};
		std::unique_ptr<LiveWait> s_live_wait;
		std::vector<std::string> s_live_seen; // battle codes watched, oldest first, kept across the resets
		std::string s_live_next_url; // the battle auto-next moved on to, "" = ZDXSV_REPLAY
		int s_live_next_left = [] {
			const char* e = std::getenv("ZDXSV_LIVE_NEXT");
			return e ? std::atoi(e) : -1; // -1 = the setting decides (no limit)
		}();
		constexpr int LIVE_NEXT_POLL_S = 5, LIVE_NEWEST_MS = 2000, LIVE_NEXT_SKIP = 32; // the skip list fits one datagram

		void LiveSeen(const std::string& code)
		{
			if (std::ranges::find(s_live_seen, code) == s_live_seen.end())
				s_live_seen.push_back(code);
		}

		void LiveTake()
		{
			const bool ok = s_live_down->Take(s_live_got, LIVE_STALL_MS);
			const size_t fb = s_players * sizeof(NetInput);
			if (s_live_got.frameBytes > 0 && static_cast<size_t>(s_live_got.frameBytes) != fb && s_live_close.empty())
				s_live_close = fmt::format("frame size {}, not {}", s_live_got.frameBytes, fb);
			if (const size_t n = s_live_got.inputs.size() / fb; n > 0 && s_live_close.empty())
			{
				const size_t at = s_play_inputs.size();
				s_play_inputs.resize(at + n * s_players);
				std::memcpy(&s_play_inputs[at], s_live_got.inputs.data(), n * fb);
				s_live_got.inputs.erase(s_live_got.inputs.begin(), s_live_got.inputs.begin() + n * fb);
				s_play_frames += static_cast<int>(n);
			}
			if (s_live_got.closed && s_live_close.empty())
				s_live_close = s_live_got.close.empty() ? "end" : s_live_got.close;
			if (!ok && s_live_close.empty())
			{
				Console.Error("ZdxsvGgpo: live: nothing from the lobby for %d s, stream lost", LIVE_STALL_MS / 1000);
				s_live_close = "lost";
			}
		}

		// The live stream as a replay file's bytes (header, frames so far, start state).
		std::optional<std::vector<u8>> LiveOpen(const std::string& url)
		{
			std::string error;
			s_live_down = Zdxsv::LiveDown::Open(url, LIVE_OPEN_MS, error);
			if (!s_live_down)
			{
				Console.Error("ZdxsvGgpo: live %s: %s", url.c_str(), error.c_str());
				return std::nullopt;
			}
			s_live_got = {};
			s_live_down->Take(s_live_got, LIVE_STALL_MS);
			const size_t fb = std::max(s_live_got.frameBytes, 1);
			const size_t frames = s_live_got.inputs.size() / fb;
			using namespace Pb;
			std::vector<u8> pb = std::move(s_live_got.header);
			PutInt(pb, 48, static_cast<int64_t>(frames));
			PutBytes(pb, 49, s_live_got.inputs.data(), frames * fb);
			s_live_got.inputs.erase(s_live_got.inputs.begin(), s_live_got.inputs.begin() + frames * fb);
			const std::string code = s_live_down->Code();
			LiveSeen(code);
			Console.WriteLn("ZdxsvGgpo: live: battle %s, %zu frames so far%s", code.c_str(), frames,
				s_live_got.closed ? ", closed" : "");
			return pb;
		}

		// PlayNext at the end of a closed stream: true = auto-next waits for the next battle (VM paused).
		bool LiveAutoNext()
		{
			if (s_live_next_left < 0 ? !Host::GetBoolSettingValue("DEV9/Eth", "ZdxsvLiveAutoNext", false) : s_live_next_left == 0)
				return false;
			if (s_live_next_left > 0)
				s_live_next_left--;
			const std::string_view src = s_live_next_url.empty() ? std::string_view(s_play_env) : std::string_view(s_live_next_url);
			const std::string_view rest = src.substr(std::min<size_t>(6, src.size())); // after udp://
			std::string host(rest.substr(0, rest.find('/')));
			Console.WriteLn("ZdxsvGgpo: live: auto-next: waiting for a new battle at %s (%zu watched)", host.c_str(), s_live_seen.size());
			s_live_wait = std::make_unique<LiveWait>();
			const auto skip_from = s_live_seen.end() - std::min<ptrdiff_t>(s_live_seen.size(), LIVE_NEXT_SKIP);
			s_live_wait->t = std::thread([w = s_live_wait.get(), host = std::move(host), seen = s_live_seen,
											 skip = std::vector<std::string>(skip_from, s_live_seen.end())]() {
				Common::Timer since;
				while (!w->quit)
				{
					// the seen check holds the pick against a lobby that ignores skip (it answers its newest battle)
					if (const std::string code = Zdxsv::LiveDown::Newest(host, skip, LIVE_NEWEST_MS);
						!code.empty() && std::ranges::find(seen, code) == seen.end())
					{
						Console.WriteLn("ZdxsvGgpo: live: auto-next: moving on to %s after %.0f s", code.c_str(), since.GetTimeSeconds());
						Host::RunOnCPUThread([code, url = fmt::format("udp://{}/{}", host, code)] {
							if (!VMManager::HasValidVM())
								return;
							LiveSeen(code);
							s_live_next_url = url;
							VMManager::Reset();
							VMManager::SetPaused(false);
						});
						return;
					}
					for (int i = 0; i < LIVE_NEXT_POLL_S * 10 && !w->quit; i++)
						Threading::Sleep(100);
				}
			});
			VMManager::SetPaused(true);
			return true;
		}

		void LiveCatchupEnd(int f)
		{
			if (!std::exchange(s_live_catchup, false))
				return;
			VMManager::SetLimiterMode(s_live_limiter);
			Console.WriteLn("ZdxsvGgpo: live: caught up at frame %d, vsync %u", f, g_FrameCount);
		}

		// Pacing (flycast GdxsvBackendReplay::UpdateFramePacing): while following the edge, g_frame_period_trim_us holds
		// the received, unplayed frames at LIVE_BUFFER instead of whole-frame waits: feedforward at the stream's
		// measured rate, plus proportional (outside a deadband) and integral terms on the buffer error. 0 while
		// catching up, seeking, taking over, after the stream closed, or with nothing received for PACE_STALL frames.
		// ZDXSV_LIVE_PACING=0 (test control): off. Every PACE_LOG frames: the gap, trim and rate in the log.
		constexpr int PACE_DEADBAND_MAX = 2, PACE_US_PER_FRAME = 40, PACE_STALL = 5, PACE_LOG = 600;
		constexpr double PACE_I_GAIN = 0.25, PACE_I_LIMIT = 600, PACE_FLOOR_US = -4000, PACE_CEIL_US = 8000;
		constexpr double PACE_WINDOW_S = 1, PACE_ALPHA = 0.25, PACE_MIN_HZ = 30, PACE_MAX_HZ = 65, PACE_IDLE_S = 0.1;
		const bool s_pace_off = [] { const char* e = std::getenv("ZDXSV_LIVE_PACING"); return e && e[0] == '0'; }();
		double s_pace_i = 0, s_pace_hz = 0;
		Common::Timer s_pace_win, s_pace_call;
		int s_pace_win_recv = -1, s_pace_last_recv = 0, s_pace_stall = 0, s_pace_log = 0;

		void LivePaceReset()
		{
			g_frame_period_trim_us = 0;
			s_pace_i = 0;
			s_pace_hz = 0;
			s_pace_win_recv = -1;
			s_pace_stall = 0;
		}

		void LivePace(int next)
		{
			if (++s_pace_log >= PACE_LOG)
			{
				s_pace_log = 0;
				Console.WriteLn("ZdxsvGgpo: live: pace frame %d gap %d trim %d us rate %.2f hz, %d waits %.0f ms", next,
					s_play_frames - next, g_frame_period_trim_us, s_pace_hz, s_live_waits, s_live_wait_ms);
			}
			const bool idle = s_pace_call.GetTimeSeconds() > PACE_IDLE_S; // paused, or a long wait at the edge
			s_pace_call.Reset();
			if (s_pace_off || s_live_catchup || s_run_load >= 0 || s_play_target >= 0 || s_to_phase != TO_OFF ||
				!s_live_close.empty())
			{
				LivePaceReset();
				return;
			}
			const int recv = s_play_frames;
			if (recv != s_pace_last_recv)
			{
				s_pace_last_recv = recv;
				s_pace_stall = 0;
			}
			else if (s_pace_stall < PACE_STALL)
				s_pace_stall++;
			if (s_pace_stall >= PACE_STALL || idle || s_pace_win_recv < 0)
			{
				// the silence is not the match's rate: restart the window, play at nominal
				g_frame_period_trim_us = 0;
				s_pace_win.Reset();
				s_pace_win_recv = recv;
				return;
			}
			const double nominal_hz = VMManager::GetFrameRate();
			if (s_pace_hz <= 0)
				s_pace_hz = nominal_hz;
			if (const double win = s_pace_win.GetTimeSeconds(); win >= PACE_WINDOW_S)
			{
				if (const double observed = (recv - s_pace_win_recv) / win; observed > 1)
					s_pace_hz = std::clamp((1 - PACE_ALPHA) * s_pace_hz + PACE_ALPHA * observed, PACE_MIN_HZ, PACE_MAX_HZ);
				s_pace_win.Reset();
				s_pace_win_recv = recv;
			}
			// positive error: further behind the edge than wanted, so a shorter period (negative trim)
			const int error = recv - next - LIVE_BUFFER;
			const double feedforward = 1e6 / s_pace_hz - 1e6 / nominal_hz;
			const int deadband = std::clamp(LIVE_BUFFER / 4, 1, PACE_DEADBAND_MAX);
			const double p = std::abs(error) > deadband ? -error * PACE_US_PER_FRAME : 0.0;
			const double unsaturated = feedforward + p + s_pace_i;
			const double step = -error * PACE_I_GAIN;
			if (!(unsaturated >= PACE_CEIL_US && step > 0) && !(unsaturated <= PACE_FLOOR_US && step < 0))
				s_pace_i = std::clamp(s_pace_i + step, -PACE_I_LIMIT, PACE_I_LIMIT);
			g_frame_period_trim_us = static_cast<s32>(std::clamp(feedforward + p + s_pace_i, PACE_FLOOR_US, PACE_CEIL_US));
		}

		// PlayNext, before frame next: the received frames taken in, the wait at the newest frame, catch-up.
		void LiveNext(int next)
		{
			LiveTake();
			if (next >= s_play_frames && s_live_close.empty())
			{
				const int want = next + (s_run_load >= 0 || s_play_target >= 0 || s_live_catchup ? 1 : LIVE_BUFFER);
				Common::Timer wait;
				while (s_play_frames < want && s_live_close.empty())
				{
					Threading::Sleep(2);
					LiveTake();
				}
				s_live_waits++;
				s_live_wait_ms += wait.GetTimeMilliseconds();
			}
			if (!s_live_close.empty() && next >= s_play_frames && !std::exchange(s_live_close_logged, true))
				Console.WriteLn("ZdxsvGgpo: live: stream closed (%s) at frame %d, %d waits %.0f ms", s_live_close.c_str(), s_play_frames,
					s_live_waits, s_live_wait_ms);
			const int ahead = s_play_frames - next;
			if (!s_live_catchup && ahead > LIVE_BUFFER + LIVE_CATCHUP && s_run_load < 0 && s_play_target < 0)
			{
				s_live_catchup = true;
				s_live_limiter = VMManager::GetLimiterMode();
				VMManager::SetLimiterMode(LimiterModeType::Unlimited);
				Console.WriteLn("ZdxsvGgpo: live: %d frames behind at frame %d: catching up", ahead, next);
			}
			else if (s_live_catchup && ahead <= LIVE_BUFFER + LIVE_EDGE)
				LiveCatchupEnd(next);
			LivePace(next);
		}

		// GgpoOnVmShutdown: the key files go; a new VM (or the reset one) loads the files again and plays from the start.
		// Four-screen (flycast's ReplayFourScreen): ZDXSV_REPLAY_FOUR=1: the host spawns
		// one guest per other position (ZDXSV_REPLAY_POV=p, ZDXSV_REPLAY_GROUP, -logfile emulog-povP.txt, the net trace
		// as <trace>-povP), tiles the windows 2x2 by position, and all hold each other on the same frame (SpectateSync).
		// A member more than SYNC_CHASE frames behind the newest seeks to it (a guest boots seconds after the host).
		// The guests follow the host's pause, speed and seeks, and quit with it. Replays only: a live stream carries
		// one position's state.
		constexpr int SYNC_WAIT_MS = 200, SYNC_CHASE = 30;
		std::thread s_sync_thread;
		std::atomic<bool> s_sync_quit{false};
		u32 s_sync_seek_gen = 0;
		int s_sync_pids[Zdxsv::SpectateSync::GRID] = {};
		bool s_sync_chase = false; // s_play_req is a chase, not a host seek for the guests
		// ZDXSV_REPLAY_SYNC=0 (test control): members publish their frame but neither wait nor chase
		const bool s_sync_test_off = [] { const char* e = std::getenv("ZDXSV_REPLAY_SYNC"); return e && e[0] == '0'; }();

		bool PlayCatchingUp() { return s_run_load >= 0 || s_play_target >= 0 || s_live_catchup; }

		// Not the CPU thread. Host: publishes its pause, tiles the windows; guest: follows the pause, quits with the host.
		void SyncWatch(bool host)
		{
			int want = 0;
			for (const int pid : s_sync_pids)
				want += pid != 0;
			bool tiled = false, quit = false;
			std::optional<bool> paused;
			for (int i = 0; !s_sync_quit; i++)
			{
				if (host)
				{
					Zdxsv::SpectateSync::HostPaused(VMManager::GetState() == VMState::Paused);
					if (!tiled && i % 25 == 0 && i < 3000) // 60 s
						tiled = Zdxsv::SpectateSync::TileWindows(s_sync_pids) == want;
				}
				else
				{
					Zdxsv::SpectateSync::Control c;
					if (!quit && Zdxsv::SpectateSync::HostGone())
					{
						quit = true;
						Console.WriteLn("ZdxsvGgpo: replay four-screen: the host is gone, quitting");
						Host::RequestVMShutdown(false, false, false);
					}
					if (Zdxsv::SpectateSync::ReadControl(c) && paused != c.paused)
					{
						paused = c.paused;
						Host::RunOnCPUThread([p = c.paused] {
							if (VMManager::HasValidVM())
								VMManager::SetPaused(p);
						});
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
		}

		void SyncStop()
		{
			s_sync_quit = true;
			if (s_sync_thread.joinable())
				s_sync_thread.join();
			Zdxsv::SpectateSync::Leave();
			std::fill(std::begin(s_sync_pids), std::end(s_sync_pids), 0);
		}

		// CPU thread, after the replay loaded as position `me`.
		void SyncStart(int me)
		{
			const char* group = std::getenv("ZDXSV_REPLAY_GROUP");
			const char* four = std::getenv("ZDXSV_REPLAY_FOUR");
			const bool host = !group && four && four[0] == '1';
			if (!group && !host)
				return;
			if (s_live_down)
			{
				Console.Error("ZdxsvGgpo: replay four-screen: replays only, not live spectating");
				return;
			}
			std::random_device rd;
			const std::string g = group ? group : fmt::format("{:08x}{:08x}", rd(), rd());
			if (!Zdxsv::SpectateSync::Join(g, host))
				return;
			Zdxsv::SpectateSync::Control c;
			Zdxsv::SpectateSync::ReadControl(c);
			s_sync_seek_gen = c.seek_gen;
			if (host)
			{
				Zdxsv::SpectateSync::HostLimiter(static_cast<int>(VMManager::GetLimiterMode()));
				s_sync_pids[me] = Zdxsv::SpectateSync::SelfPid();
				const std::string trace = std::getenv("ZDXSV_NET_TRACE") ? std::getenv("ZDXSV_NET_TRACE") : "";
				for (int p = 0; p < s_players && p < Zdxsv::SpectateSync::GRID; p++)
				{
					if (p == me || !s_play_pov_ok[p])
						continue;
					std::vector<std::pair<std::string, std::string>> env = {
						{"ZDXSV_REPLAY_POV", std::to_string(p)}, {"ZDXSV_REPLAY_GROUP", g}, {"ZDXSV_REPLAY_FOUR", "0"}};
					if (!trace.empty())
					{
						const size_t dot = trace.find_last_of("./\\");
						const bool ext = dot != std::string::npos && trace[dot] == '.';
						env.emplace_back("ZDXSV_NET_TRACE", ext ? fmt::format("{}-pov{}{}", trace.substr(0, dot), p, trace.substr(dot)) :
						                                          fmt::format("{}-pov{}", trace, p));
					}
					s_sync_pids[p] = Zdxsv::SpectateSync::SpawnGuest(env, {"-logfile", Path::Combine(EmuFolders::Logs, fmt::format("emulog-pov{}.txt", p))});
					Console.WriteLn("ZdxsvGgpo: replay four-screen: group %s, point of view %d in pid %d", g.c_str(), p, s_sync_pids[p]);
				}
			}
			s_sync_quit = false;
			s_sync_thread = std::thread(SyncWatch, host);
		}

		// CPU thread, PlayNext: the host's seek and speed for a guest; a seek to the newest member when far behind.
		void SyncFollow(int next)
		{
			if (!Zdxsv::SpectateSync::Active())
				return;
			Zdxsv::SpectateSync::Control c;
			if (!Zdxsv::SpectateSync::IsHost() && Zdxsv::SpectateSync::ReadControl(c))
			{
				if (c.seek_gen != s_sync_seek_gen)
				{
					s_sync_seek_gen = c.seek_gen;
					s_play_req = c.seek_target;
					Console.WriteLn("ZdxsvGgpo: replay four-screen: the host seeked to frame %d", c.seek_target);
				}
				if (!PlayCatchingUp() && c.limiter >= 0 && c.limiter != static_cast<int>(VMManager::GetLimiterMode()))
					VMManager::SetLimiterMode(static_cast<LimiterModeType>(c.limiter));
			}
			if (const int lead = Zdxsv::SpectateSync::Leader();
				!s_sync_test_off && !PlayCatchingUp() && s_to_phase == TO_OFF && s_play_req == INT_MIN && lead - next > SYNC_CHASE)
			{
				Console.WriteLn("ZdxsvGgpo: replay four-screen: frame %d, %d behind: seeking to %d", next, lead - next, lead + SYNC_CHASE / 3);
				s_sync_chase = true;
				s_play_req = lead + SYNC_CHASE / 3; // the newest goes on during the seek
			}
		}

		// CPU thread, before frame `next` plays: holds it for a member behind, publishes the host's speed.
		void SyncFrame(int next)
		{
			if (!Zdxsv::SpectateSync::Active())
				return;
			if (PlayCatchingUp())
			{
				Zdxsv::SpectateSync::Publish(next, true);
				return;
			}
			if (Zdxsv::SpectateSync::IsHost())
				Zdxsv::SpectateSync::HostLimiter(static_cast<int>(VMManager::GetLimiterMode()));
			if (!s_sync_test_off)
				Zdxsv::SpectateSync::WaitForPeers(next, SYNC_WAIT_MS);
			else
				Zdxsv::SpectateSync::Publish(next, false);
			if (next % 300 == 0)
			{
				int slowest, newest;
				const int n = Zdxsv::SpectateSync::Members(slowest, newest);
				Console.WriteLn("ZdxsvGgpo: replay four-screen: frame %d, %d members, spread %d (%d..%d)", next, n,
					newest >= 0 ? newest - slowest : -1, slowest, newest);
			}
		}

		void PlayReset()
		{
			SyncStop();
			s_sync_chase = false;
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
			s_live_down.reset();
			LivePaceReset();
			s_live_wait.reset();
			s_live_got = {};
			s_live_close.clear();
			s_live_catchup = s_live_close_logged = false;
			s_live_waits = 0;
			s_live_wait_ms = 0;
			s_play_inputs.clear();
			s_play_frames = 0;
			s_play_hashes.clear();
			s_play_rngs.clear();
			s_play_rng_pos = -1;
			s_play_hash_checks = s_play_hash_bad = s_play_rng_checks = s_play_rng_bad = s_play_hash_cut = 0;
			s_play_hash_first_bad = -1;
			s_play_cut.clear();
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
			s_play_rearm_seek = -1;
			s_play_booted_saved = false;
			s_play_pov_req = -1;
			s_play_pov_at.clear();
			std::fill(std::begin(s_play_sent), std::end(s_play_sent), PlaySent{});
			s_bar_frame = -1;
			s_bar_frames = 0;
			s_bar_pov = s_bar_target = -1;
			s_bar_povs = 0;
		}

		// A replay file (Zdxsv/replay.proto) as PlayLoadFile uses it.
		struct ReplayFile
		{
			s64 version = 0, players = -1, me = -1, frames = -1, input_size = -1;
			std::string code;
			std::vector<u8> rx0;
			std::vector<int64_t> hle0;
			std::string_view inputs; // into the file's bytes
			std::vector<std::vector<u8>> answers; // lobby frames, 12-byte header + body
			std::string_view hashes; // u32 per frame, "" = none (older files)
			std::optional<u32> start_rng;
			std::vector<int64_t> load_frames, load_rngs;
			std::vector<int64_t> play_starts, game_ends, round_ends;
		};

		bool ReplayParse(std::string_view all, ReplayFile& r)
		{
			Pb::Reader rd{reinterpret_cast<const uint8_t*>(all.data()), reinterpret_cast<const uint8_t*>(all.data() + all.size())};
			return rd.Fields([&r](uint32_t field, uint32_t wt, uint64_t v, const uint8_t* b, size_t n) {
				const std::string_view bytes(reinterpret_cast<const char*>(b), n);
				if (wt == 0)
				{
					s64* num = field == 4 ? &r.version : field == 40 ? &r.players : field == 41 ? &r.me : field == 47 ? &r.input_size :
							   field == 48 ? &r.frames : nullptr;
					if (num)
						*num = static_cast<s64>(v);
					else if (field == 46)
						r.hle0.push_back(static_cast<int64_t>(v));
					else if (field == 53)
						r.start_rng = static_cast<u32>(v);
					else if (field == 54)
						r.load_frames.push_back(static_cast<int64_t>(v));
					else if (field == 55)
						r.load_rngs.push_back(static_cast<int64_t>(v));
					else if (field == 57)
						r.play_starts.push_back(static_cast<int64_t>(v));
					else if (field == 58)
						r.game_ends.push_back(static_cast<int64_t>(v));
					else if (field == 59)
						r.round_ends.push_back(static_cast<int64_t>(v));
				}
				else if (wt == 2)
				{
					if (field == 3)
						r.code = bytes;
					else if (field == 45)
						r.rx0.assign(b, b + n);
					else if (field == 46)
						return Pb::ReadInts(wt, v, b, n, r.hle0);
					else if (field == 49)
						r.inputs = bytes;
					else if (field == 51 && n >= 12)
						r.answers.emplace_back(b, b + n);
					else if (field == 52)
						r.hashes = bytes;
					else if (field == 54)
						return Pb::ReadInts(wt, v, b, n, r.load_frames);
					else if (field == 55)
						return Pb::ReadInts(wt, v, b, n, r.load_rngs);
					else if (field == 57)
						return Pb::ReadInts(wt, v, b, n, r.play_starts);
					else if (field == 58)
						return Pb::ReadInts(wt, v, b, n, r.game_ends);
					else if (field == 59)
						return Pb::ReadInts(wt, v, b, n, r.round_ends);
				}
				return true;
			});
		}

		std::optional<std::vector<u8>> HttpGet(const std::string& url)
		{
			std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create(Host::GetHTTPUserAgent());
			if (!http)
			{
				Console.Error("ZdxsvGgpo: replay %s: no HTTP client", url.c_str());
				return std::nullopt;
			}
			http->SetTimeout(120.0f);
			std::optional<std::vector<u8>> got;
			http->CreateRequest(url, [&](s32 status, const std::string&, HTTPDownloader::Request::Data data) {
				if (status == HTTPDownloader::HTTP_STATUS_OK)
					got = std::move(data);
				else
					Console.Error("ZdxsvGgpo: replay %s: status %d", url.c_str(), status); // 204: no such battle
			});
			http->WaitForAllRequests();
			return got;
		}

		// The first "replay_url" string of a JSON text (Go's encoder: \" \\ \/ and \u00XX escapes), empty if none.
		std::string JsonReplayUrl(std::string_view json)
		{
			const size_t key = json.find("\"replay_url\"");
			if (key == std::string_view::npos)
				return {};
			size_t i = json.find_first_not_of(" \t\r\n", key + 12);
			if (i == std::string_view::npos || json[i] != ':' || (i = json.find_first_not_of(" \t\r\n", i + 1)) == std::string_view::npos ||
				json[i] != '"')
				return {};
			std::string url;
			for (i++; i < json.size() && json[i] != '"'; i++)
			{
				if (json[i] != '\\')
					url += json[i];
				else if (i + 1 < json.size() && json[i + 1] == 'u' && i + 5 < json.size())
				{
					const std::optional<u32> c = StringUtil::FromChars<u32>(json.substr(i + 2, 4), 16);
					if (!c || *c >= 0x80) // a URL is ASCII
						return {};
					url += static_cast<char>(*c);
					i += 5;
				}
				else if (i + 1 < json.size())
					url += json[++i];
			}
			return i < json.size() ? url : std::string();
		}

		// ZDXSV_REPLAY=http(s)://...: a replay file, or the lobby's /lbs/replay?battle_code=C answer (as gdxsv
		// lbsapi: a JSON list, newest first), whose first battle's replay_url is then fetched.
		std::optional<std::vector<u8>> HttpOpen(const std::string& url)
		{
			std::optional<std::vector<u8>> got = HttpGet(url);
			if (!got || got->empty() || got->front() != '[') // a .pb starts with field 1 (0x08)
				return got;
			const std::string pb = JsonReplayUrl(std::string_view(reinterpret_cast<const char*>(got->data()), got->size()));
			if (pb.empty())
			{
				Console.Error("ZdxsvGgpo: replay %s: no replay_url in the answer", url.c_str());
				return std::nullopt;
			}
			Console.WriteLn("ZdxsvGgpo: replay %s: replay_url %s", url.c_str(), pb.c_str());
			return HttpGet(pb);
		}

		// Reads the file; returns its recorder's position, -1 = not used. One file holds every position's inputs, so it
		// plays any point of view: each starts from the common state with its own lobby answers (PlayCommonStart).
		int PlayLoadFile(const std::string& path)
		{
			const bool live = path.starts_with("udp://");
			const bool http = path.starts_with("http://") || path.starts_with("https://");
			if (s_play_frames > 0 || s_live_down)
			{
				Console.Error("ZdxsvGgpo: replay %s: one file plays every position, a 2nd one is not used", path.c_str());
				return -1;
			}
			const std::optional<std::vector<u8>> file =
				live ? LiveOpen(path.substr(6)) : http ? HttpOpen(path) : FileSystem::ReadBinaryFile(Path::ToNativePath(path).c_str()); // '/' fails on Windows
			const std::string_view all = file ? std::string_view(reinterpret_cast<const char*>(file->data()), file->size()) : std::string_view();
			ReplayFile r;
			if (all.empty() || !ReplayParse(all, r) || r.version <= 0)
			{
				Console.Error("ZdxsvGgpo: replay %s: not a replay file", path.c_str());
				return -1;
			}
			const s64 players = r.players, me = r.me, frames = r.frames;
			// each bound before the next product: the values are any s64
			if (players < 1 || players > GGPO_MAX_PLAYERS || me < 0 || me >= players || frames < 1 || frames > INT_MAX ||
				r.input_size != static_cast<s64>(sizeof(NetInput)) ||
				r.inputs.size() != static_cast<u64>(frames) * players * sizeof(NetInput))
			{
				Console.Error("ZdxsvGgpo: replay %s: bad header or short file (players %lld position %lld frames %lld)", path.c_str(),
					players, me, frames);
				return -1;
			}
			if (r.answers.empty())
			{
				Console.Error("ZdxsvGgpo: replay %s: no lobby answers (an older file with a start state is not played)", path.c_str());
				return -1;
			}
			int h[9] = {};
			if (r.hle0.size() != std::size(h))
			{
				Console.Error("ZdxsvGgpo: replay %s: hle0 has %zu values, not %zu", path.c_str(), r.hle0.size(), std::size(h));
				return -1;
			}
			std::copy(r.hle0.begin(), r.hle0.end(), h);
			// The kind-3 counters index s_zds_k3 (NetApply) and the file holds no kind-3 bodies: only the empty barrier
			// the recorder writes (ReplayBegin, right after the session reset) can be played.
			if (std::any_of(h + 4, h + 9, [](int v) { return v != 0; }))
			{
				Console.Error("ZdxsvGgpo: replay %s: hle0 kind-3 counters %d,%d,%d,%d,%d not 0", path.c_str(), h[4], h[5], h[6], h[7], h[8]);
				return -1;
			}
			s_play_common = true;
			s_play_answers = std::move(r.answers);
			s_play_inputs.resize(static_cast<size_t>(frames * players));
			std::memcpy(s_play_inputs.data(), r.inputs.data(), r.inputs.size());
			s_play_frames = static_cast<int>(frames);
			s_play_hashes.resize(r.hashes.size() == static_cast<u64>(frames) * sizeof(u32) ? static_cast<size_t>(frames) : 0);
			std::memcpy(s_play_hashes.data(), r.hashes.data(), s_play_hashes.size() * sizeof(u32));
			s_play_rngs.clear();
			if (r.start_rng)
				s_play_rngs[0] = *r.start_rng;
			for (size_t i = 0; i < r.load_frames.size() && i < r.load_rngs.size(); i++)
				s_play_rngs[static_cast<int>(r.load_frames[i])] = static_cast<u32>(r.load_rngs[i]);
			s_play_cut.clear();
			for (const int64_t e : r.game_ends)
			{
				int64_t to = frames;
				for (const int64_t s : r.play_starts)
					if (s > e)
						to = std::min(to, s);
				s_play_cut.emplace_back(static_cast<int>(e), static_cast<int>(to));
			}
			for (size_t i = 0; i + 1 < r.round_ends.size(); i += 2)
				s_play_cut.emplace_back(static_cast<int>(r.round_ends[i]), static_cast<int>(r.round_ends[i + 1]));
			s_play_rng_pos = static_cast<int>(me);
			s_players = static_cast<int>(players);
			// key 0 of each position: its state is saved at its common start's arm (PlayCommonStart); the HLE state
			// here (the recorder's) is only compared there
			for (int p = 0; p < s_players; p++)
			{
				auto k = std::make_unique<PlayKey>();
				k->path = Path::Combine(EmuFolders::Cache, fmt::format("zdxsv-replay-p{}.p2s", p));
				s_play_keys[p].emplace(0, std::move(k));
				s_play_pov_ok[p] = true;
			}
			RollbackState& rb = s_play_keys[me].at(0)->rb;
			rb.net_rx.assign(r.rx0.begin(), r.rx0.end());
			rb.ps = {static_cast<u8>(h[0]), static_cast<u8>(h[1]), h[2] != 0, h[3] != 0};
			for (int p = 0; p < 4; p++)
				rb.zds_seen[p] = h[4 + p];
			rb.zds_rel = h[8];
			Console.WriteLn("ZdxsvGgpo: replay %s: recorded at position %lld of %d, %lld frames, rx0 %zu bytes, "
							"%zu lobby answers, state hashes %s, rngs %zu", path.c_str(), me, s_players, frames,
				rb.net_rx.size(), s_play_answers.size(), r.hashes.empty() ? "no" : "yes",
				(r.start_rng ? 1 : 0) + std::min(r.load_frames.size(), r.load_rngs.size()));
			if (s_net_trace) // battle start marker: pwcheck.py --battle splits a multi-battle trace (live auto-next)
				std::fprintf(s_net_trace, "%u B %s\n", g_FrameCount, r.code.empty() ? "-" : r.code.c_str());
			return static_cast<int>(me);
		}

		// The hosted post-entry state of a common start (as gdxsv's slot 99): ZDXSV_REPLAY_STATE, else the setting
		// DEV9/Eth ZdxsvReplayStateUrl (default: the hosted one); a URL is downloaded once into the cache (one file
		// per URL). Set empty = the booted state is used as it is.
		bool PlayCommonState()
		{
			const char* env = std::getenv("ZDXSV_REPLAY_STATE");
			const std::string src = env ? env : Host::GetStringSettingValue("DEV9/Eth", "ZdxsvReplayStateUrl", REPLAY_STATE_URL);
			std::string path = src;
			if (src.empty()) // the booted state, saved at the first start for a switch's start
			{
				path = Path::Combine(EmuFolders::Cache, "zdxsv-common-booted.p2s");
				if (!std::exchange(s_play_booted_saved, true))
				{
					Error error;
					std::unique_ptr<ArchiveEntryList> list = SaveState_DownloadState(&error);
					if (!list || !SaveState_ZipToDisk(std::move(list), nullptr, path.c_str(), &error))
					{
						Console.Error("ZdxsvGgpo: replay: booted state save failed: %s", error.GetDescription().c_str());
						s_play_booted_saved = false;
					}
					return true;
				}
			}
			if (src.starts_with("http://") || src.starts_with("https://"))
			{
				path = Path::Combine(EmuFolders::Cache, fmt::format("zdxsv-common-{:016x}.p2s", std::hash<std::string>{}(src)));
				if (!FileSystem::FileExists(path.c_str()))
				{
					const std::optional<std::vector<u8>> got = HttpGet(src);
					if (!got || !FileSystem::WriteBinaryFile(path.c_str(), got->data(), got->size()))
					{
						Console.Error("ZdxsvGgpo: replay: common state %s: download to %s failed", src.c_str(), path.c_str());
						return false;
					}
					Console.WriteLn("ZdxsvGgpo: replay: common state %s: %zu bytes into %s", src.c_str(), got->size(), path.c_str());
				}
			}
			Error error;
			if (!VMManager::LoadState(path.c_str(), &error))
			{
				Console.Error("ZdxsvGgpo: replay: common state %s: load failed: %s", path.c_str(), error.GetDescription().c_str());
				return false;
			}
			Console.WriteLn("ZdxsvGgpo: replay: common state %s loaded", path.c_str());
			return true;
		}

		// CPU thread, queued at the first vsync.
		void PlayLoad()
		{
			int me = -1;
			const std::string paths = s_live_next_url.empty() ? s_play_env : s_live_next_url; // the split's views point into it
			for (const std::string_view path : StringUtil::SplitString(paths, ';'))
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
					Console.Error("ZdxsvGgpo: replay: ZDXSV_REPLAY_POV=%s is not a position of the battle, playing position %d", e, me);
			}
			PlayCommonArm(me);
		}

		// The battle start from the common state, the lobby answering 0x6912 (own position) with pov: the game plays that
		// position from its arm on. PlayLoad, and a switch to a position with no key yet (PlaySwitch), which then seeks.
		bool PlayCommonArm(int pov, int seek)
		{
			if (!PlayCommonState())
				return false;
			s_play_common = true;
			s_play_rearm_seek = seek;
			if (seek >= 0)
			{
				s_play_rearm_limiter = s_play_target >= 0 ? s_play_limiter : VMManager::GetLimiterMode();
				s_play_target = -1;
				VMManager::SetLimiterMode(LimiterModeType::Unlimited); // the lobby phase up to the arm
			}
			s_started = s_net_armed = s_frame_ended = false;
			g_ggpo_active = false;
			// the battle socket's HLE state as at the first start (the arm adds the lobby frames to net_rx)
			s_rb = {};
			s_rings = {};
			s_net_frame = 0;
			s_net_end = -1;
			s_zd_steps = s_zd_changed = s_zds_echo = s_zds_skip = s_zds_k3rel = 0;
			for (auto& k3 : s_zds_k3)
				k3.clear();
			s_net = true;
			s_net_me = pov;
			s_rbk = true;
			s_rbk_me = pov;
			s_rbk_n = s_players;
			RbkReset();
			Console.WriteLn("ZdxsvGgpo: replay: common start as position %d of %d, %zu lobby answers%s", pov, s_players,
				s_play_answers.size(), seek >= 0 ? fmt::format(", then seek to {}", seek).c_str() : "");
			return true;
		}

		// Returned at the arm of a common start = GGPO frame 0 as ReplayBegin sees it live: this state becomes key 0
		// of the point of view, then the replay starts from it as from a file's state.
		void PlayCommonStart()
		{
			s_play_common = false;
			const int me = s_net_me;
			PlayKey& k0 = *s_play_keys[me].at(0);
			const bool hle_same = k0.rb.net_rx == s_rb.net_rx && k0.rb.ps.n == s_rb.ps.n && k0.rb.ps.rel == s_rb.ps.rel &&
								  k0.rb.ps.hold == s_rb.ps.hold && k0.rb.ps.go == s_rb.ps.go;
			Error error;
			std::unique_ptr<ArchiveEntryList> list = SaveState_DownloadState(&error);
			if (!list || !SaveState_ZipToDisk(std::move(list), nullptr, k0.path.c_str(), &error))
			{
				Console.Error("ZdxsvGgpo: replay: common start: state save failed: %s", error.GetDescription().c_str());
				return;
			}
			k0.rb = s_rb;
			k0.ok = true;
			// the file's HLE state is its recorder's position's
			Console.WriteLn("ZdxsvGgpo: replay: common start armed at vsync %u as position %d (asked %d), frame 0 HLE state %s",
				g_FrameCount, me, s_rbk_me, me != s_play_rng_pos ? "not compared" : hle_same ? "equals the file's" : "differs from the file's");
			// PlayBegin loads that state as PlayLoad does
			s_started = false;
			g_ggpo_active = false;
			Host::RunOnCPUThread([me] { PlayBegin(me); });
		}

		void PlayBegin(int me)
		{
			const PlayKey& k0 = *s_play_keys[me].at(0);
			Error error;
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
			if (const int seek = std::exchange(s_play_rearm_seek, -1); seek >= 0)
			{
				// a switch: the rest of the first start (options, four-screen) stays
				VMManager::SetLimiterMode(s_play_rearm_limiter);
				Console.WriteLn("ZdxsvGgpo: replay: point of view %d from its key 0, seeking to %d", me, seek);
				s_play_req = seek;
				PlayFrame(0);
				return;
			}
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
			SyncStart(me);
			PlayFrame(0);
		}

		// Returns the frame to run next.
		int PlaySwitch(int pov, int target)
		{
			if (pov < 0 || pov >= s_players || !s_play_pov_ok[pov])
			{
				Console.Error("ZdxsvGgpo: replay: no point of view %d in the battle", pov);
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
			if (!s_play_keys[pov].at(0)->ok) // never played: its own battle start first
			{
				if (PlayCommonArm(pov, std::clamp(target, 0, s_play_frames - 1)))
					return -2;
				Console.Error("ZdxsvGgpo: replay: point of view %d: no common state", pov);
				swap_sent(pov);
				swap_sent(old);
				s_net_me = old;
				return s_net_frame + 1;
			}
			const int next = PlaySeek(target, true);
			if (next >= 0)
				return next;
			swap_sent(pov);
			swap_sent(old);
			s_net_me = old;
			return s_net_frame + 1;
		}

		// Takeover requests at the frame end before `next` runs (may set it). Returns true: pause after it.
		bool PlayTakeover(int& next)
		{
			int req = s_to_req.exchange(TO_REQ_NONE);
			if (next == s_to_test_at)
				s_to_test_at = -1, req = TO_REQ_TAKE;
			if (next == s_to_test_retry && s_to_phase == TO_ON)
				s_to_test_retry = -1, req = TO_REQ_RETRY;
			if (s_to_phase == TO_ON && !s_to_test)
			{
				const bool start = s_host[PadDualshock2::Inputs::PAD_START] >= 0.5f;
				if (start && !s_to_start_held)
					req = TO_REQ_RETRY;
				s_to_start_held = start;
			}
			const int phase = s_to_phase;
			const bool aligning = phase == TO_ALIGN || phase == TO_COUNT;
			const auto align = [] {
				s_to_phase = TO_ALIGN;
				s_to_skip = false;
				s_to_start_held = s_host[PadDualshock2::Inputs::PAD_START] >= 0.5f; // skip needs a new press
				Console.WriteLn("ZdxsvGgpo: replay takeover: hold the replay's input %04x at frame %d", s_to_target.load(), s_to_frame);
			};
			const auto load = [&next] {
				const int t = TakeoverLoad();
				if (t < 0)
					TakeoverOff("failed");
				else
					next = t;
				return t >= 0;
			};
			if (req == TO_REQ_TAKE && phase == TO_OFF)
			{
				if (s_live_down)
					Console.Error("ZdxsvGgpo: replay takeover: not while spectating live");
				else if (TakeoverBegin(next))
				{
					PlayRunEnd("cancelled by a takeover", s_net_frame);
					if (!s_to_test)
					{
						align();
						return true;
					}
					TakeoverStart(); // the running state is the one before T
				}
			}
			else if (req == TO_REQ_START && aligning)
			{
				if (load())
					TakeoverStart();
			}
			else if (req == TO_REQ_RETRY && phase == TO_ON)
			{
				Console.WriteLn("ZdxsvGgpo: replay takeover: retry at frame %d", s_net_frame);
				if (load())
				{
					if (s_to_test)
						TakeoverStart();
					else
					{
						align();
						return true;
					}
				}
			}
			else if (req == TO_REQ_RETURN && phase != TO_OFF)
			{
				const bool ok = load();
				TakeoverOff("back to the replay");
				return ok;
			}
			else if (req == TO_REQ_CANCEL && aligning)
				TakeoverOff("cancelled");
			return false;
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
			const bool pause = PlayTakeover(next);
			if (s_to_phase != TO_OFF) // no seek, round jump or point of view switch
			{
				s_play_req = INT_MIN;
				s_play_pov_req = -1;
				s_play_round_req = INT_MIN;
			}
			SyncFollow(next);
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
			if (const bool chase = std::exchange(s_sync_chase, false); req != INT_MIN && !chase && Zdxsv::SpectateSync::IsHost())
				Zdxsv::SpectateSync::HostSeek(req);
			if (req != INT_MIN || run >= 0 || (pov >= 0 && pov != s_net_me))
			{
				PlayRunEnd("cancelled by a seek", s_net_frame);
				LiveCatchupEnd(s_net_frame);
			}
			if (req == 0 && s_battle_loads.size() > 1)
				req = s_battle_loads[1]; // from the start = from the briefing
			if (pov >= 0 && pov != s_net_me)
			{
				if (next = PlaySwitch(pov, req != INT_MIN ? req : next); next == -2)
					return; // PlayCommonStart plays on
			}
			else if (req != INT_MIN)
				next = PlaySeek(req);
			if (req == 0 && next == 0 && s_play_skip_ms)
				run = 1; // briefing not reached yet
			if (run >= 0)
				PlayRunBegin(run);
			if (s_live_down)
				LiveNext(next);
			if (next >= s_play_frames)
				PlayRunEnd("replay ended first", s_net_frame);
			const char* exit_env = std::getenv("ZDXSV_REPLAY_EXIT");
			if (next < s_play_frames || (s_to_phase == TO_ON && !(exit_env && exit_env[0] == '1')))
			{
				s_play_at_end = false;
				s_live_wait.reset(); // played on from the end: no move to another battle
				SyncFrame(next);
				PlayFrame(next);
				if (pause)
					VMManager::SetPaused(true);
			}
			else if (s_live_down && !s_play_at_end && LiveAutoNext())
			{
				s_play_at_end = true;
				PlayReport("done, auto-next"); // the next battle's VM reset drops this one's stats and pw hashes
			}
			else if (const char* e = std::getenv("ZDXSV_REPLAY_EXIT"); s_play_at_end || (e && e[0] == '1'))
				PlayStop("end");
			else
			{
				// a seek requested while paused here plays on; resuming without one stops the replay
				s_play_at_end = true;
				Console.WriteLn("ZdxsvGgpo: replay at its end (frame %d of %d, vsync %u), paused", s_net_frame, s_play_frames, g_FrameCount);
				VMManager::SetPaused(true); // now: a queued pause lets one more frame run, which ends the replay
			}
		}
	} // namespace

	namespace
	{
		// ZDXSV_RBK: answers of a real start (traced with 4 players and tests/zdxsv/fake_lobby.py): lobby frames are
		// `18 cat cmd size seq 00ffffff body` (BE, 12-byte header), the game's `81 01 ..`.
		std::vector<u8> s_rbk_rx; // what recv 0x13 / 0x14 and the poll's readable count see
		bool s_rbk_started = false; // 0x6910 (battle start) queued
		u32 s_rbk_calls[0x50] = {};
		constexpr u32 RBK_FNO_RECV_LOBBY = 0x13;

		// A reset VM (live auto-next) starts its battle from the lobby phase again.
		void RbkReset()
		{
			s_rbk_rx.clear();
			s_rbk_started = false;
			std::fill(std::begin(s_rbk_calls), std::end(s_rbk_calls), 0u);
		}
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
			Zdxsv::LobbyNoteFrame(&*(s_rbk_rx.end() - 12 - body.size()), 12 + body.size());
		}

		// Answer body of lobby Q cmd (body q). Position p (1-based) of N plays the recorded 4-player
		// position of its side: p <= ceil(N/2) side 1 (recorded 1, 2), else side 2 (recorded 3, 4).
		std::vector<u8> RbkBody(u16 cmd, const u8* q, u32 qn)
		{
			const int p = qn ? q[0] : 0;
			const int half = (s_rbk_n + 1) / 2;
			// Common start: the file's answer to cmd (0x6913 / 0x6917: the one of position p, its body's first
			// byte); the side 0x6912 stays the point of view
			if (s_play_common && cmd >= 0x6911 && cmd <= 0x6917 && cmd != 0x6912)
			{
				for (const std::vector<u8>& a : s_play_answers)
					if (a[2] == (cmd >> 8) && a[3] == (cmd & 0xff) && ((cmd != 0x6913 && cmd != 0x6917) || (a.size() > 12 && a[12] == p)))
						return std::vector<u8>(a.begin() + 12, a.end());
				Console.Error("ZdxsvGgpo: replay: common start: the file has no answer to 0x%04x (position %d)", cmd, p);
			}
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

		// Sock-0 RPC fno before GGPO arms. Results as recorded in that trace: send 0, poll as the battle
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
		// position; else the battle connection is cut (LobbyCutCall, logged once per battle info). Without a
		// battle info it is not a lobby battle: the call goes to the IOP.
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
				if (!s_lobby_logged && s_lobby_info) // else not a lobby battle (key msgs can come before the login)
				{
					Console.WriteLn("ZdxsvGgpo: lobby battle connection cut: %s (position %d, %d players, vsync %u)",
						why, me, n, g_FrameCount);
					s_lobby_cut = true;
					s_cut_sends = 0;
					report("cut");
					s_report += std::string("reason=") + why + "\n";
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
					Console.WriteLn("ZdxsvGgpo: lobby battle connection cut: %d of %d peers answered the ping test (position %d, waited %.1f s, vsync %u)",
						up, n - 1, me, wait.GetTimeSeconds(), g_FrameCount);
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
		// lobby=1, no GGPO session (LobbyArm): a connection failure, no fallback to the battle server.
		// Sends are dropped, nothing to recv, and the poll fails (-1). The battle's net pump (0x3133f0) sets
		// its net state to 9 (error) on a failed poll, so the game closes the sock and goes back to the lobby
		// at once instead of after its no-response timeout. That close goes to the IOP and ends the cut.
		bool LobbyCutCall(u32 fno, s16 sock, s16 len)
		{
			u8* ram = eeMem->Main;
			s32 result = 0;
			if (sock == NET_BATTLE_SOCK && fno == NET_FNO_SEND)
				result = std::clamp<s32>(len, 0, 0x3ca), s_cut_sends++;
			else if (sock == NET_BATTLE_SOCK && fno == NET_FNO_POLL)
				result = -1;
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
			Console.WriteLn("ZdxsvGgpo: replay: no other point of view (1 player)");
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
		else if (s_to_phase == TO_ALIGN || s_to_phase == TO_COUNT)
			ReplayTakeoverCancel();
		else
			VMManager::SetPaused(VMManager::GetState() != VMState::Paused);
	}

	void ReplayTakeover()
	{
		if (!s_play_env || s_play_frames <= 0 || !g_ggpo_active)
			return;
		if (s_to_phase == TO_OFF || s_to_phase == TO_ON)
		{
			s_to_req = s_to_phase == TO_OFF ? TO_REQ_TAKE : TO_REQ_RETRY;
			if (VMManager::GetState() == VMState::Paused)
				VMManager::SetPaused(false);
		}
	}

	void ReplayTakeoverSkip()
	{
		if (s_to_phase != TO_ALIGN && s_to_phase != TO_COUNT)
			return;
		s_to_skip = true;
		s_to_phase = TO_COUNT;
		s_to_count_t0 = Common::Timer::GetCurrentValue();
	}

	void ReplayTakeoverCancel()
	{
		if (s_to_phase != TO_ALIGN && s_to_phase != TO_COUNT)
			return;
		s_to_req = TO_REQ_CANCEL;
		VMManager::SetPaused(false);
	}

	void ReplayTakeoverReturn()
	{
		if (s_to_phase == TO_OFF)
			return;
		s_to_req = TO_REQ_RETURN;
		VMManager::SetPaused(false);
	}

	static std::atomic<u16> s_to_current{0};
	static std::atomic<float> s_to_left{0.0f};

	bool ReplayTakeoverInfo(int& phase, u16& target, u16& current, float& countdown)
	{
		if (s_bar_frame < 0)
			return false;
		phase = s_to_phase;
		target = s_to_target;
		current = s_to_current;
		countdown = s_to_left;
		return true;
	}

	void ReplayTakeoverIdle()
	{
		const int phase = s_to_phase;
		if (phase != TO_ALIGN && phase != TO_COUNT)
			return;
		u16 a, b;
		ZdPadAB(HostInput(), a, b);
		s_to_current = b;
		const bool start = s_host[PadDualshock2::Inputs::PAD_START] >= 0.5f;
		if (start && !s_to_start_held)
			ReplayTakeoverSkip();
		s_to_start_held = start;
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		if (s_to_phase == TO_ALIGN && b == s_to_target)
		{
			s_to_phase = TO_COUNT;
			s_to_count_t0 = now;
		}
		else if (s_to_phase == TO_COUNT && !s_to_skip && b != s_to_target)
			s_to_phase = TO_ALIGN;
		const double ms = s_to_phase == TO_COUNT ? Common::Timer::ConvertValueToMilliseconds(now - s_to_count_t0) : 0.0;
		s_to_left = s_to_phase == TO_COUNT ? static_cast<float>(std::max(0.0, 1.0 - ms / 1000.0)) : 1.0f;
		if (s_to_phase == TO_COUNT && ms >= 1000.0)
		{
			Console.WriteLn("ZdxsvGgpo: replay takeover: input %s, starting", s_to_skip ? "matching skipped" : "matched");
			s_to_req = TO_REQ_START;
			VMManager::SetPaused(false);
		}
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
				s_rb.net_rx.insert(s_rb.net_rx.end(), s_rbk_rx.begin(), s_rbk_rx.end());
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
			while (n + 1 < s_rb.net_rx.size() && s_rb.net_rx[n] >= 2 && n + s_rb.net_rx[n] <= s_rb.net_rx.size() && n + s_rb.net_rx[n] <= NET_RX_MAX)
				n += s_rb.net_rx[n], s_ns.rxmsgs++;
			if (n == 0 && !s_rb.net_rx.empty() && s_rb.net_rx.size() <= NET_RX_MAX) // unframed rest
				n = static_cast<u32>(s_rb.net_rx.size());
			std::memcpy(d, s_rb.net_rx.data(), n);
			s_rb.net_rx.erase(s_rb.net_rx.begin(), s_rb.net_rx.begin() + n);
			s_ns.rxbytes += n;
			result = static_cast<s32>(n);
		}
		else if (fno == NET_FNO_POLL)
		{
			// As a real battle poll returns: state 4, 0x2000 send space, readable bytes
			s_ns.polls++;
			*reinterpret_cast<u16*>(ram + NET_REQ_LEN) = 4;
			*reinterpret_cast<u16*>(ram + NET_REQ_DATA + 2) = 0x2000;
			*reinterpret_cast<u16*>(ram + NET_REQ_DATA + 4) = static_cast<u16>(std::min<size_t>(s_rb.net_rx.size(), NET_RX_MAX));
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
			s_replay_rec ? 1 : 0, keys);
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
		RbkReset();
		g_ggpo_in_rollback = false;
		s_vsyncs = 0; // the next VM start or the reset VM parses the options again (and loads a replay again)
	}
} // namespace Zdxsv
