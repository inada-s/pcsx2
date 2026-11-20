// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Shared by the Ggpo*.cpp and Replay*.cpp files only.

#pragma once

#include "Zdxsv/Ggpo.h"
#include "Zdxsv/DeltaState.h"
#include "Zdxsv/DeltaFreeze.h"
#include "Zdxsv/Lobby.h"
#include "Zdxsv/Proto.h"
#include "Zdxsv/SpectateSync.h"
#include "Zdxsv/Settings.h"
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
	// ZDXSV_REPLAY=file.pb: plays a saved replay (PlayLoad), the net=1 hooks on, no GGPO session. Else the replay
	// list's pick for one VM. Set by GgpoOnVmInitialize.
	struct PlaySource
	{
		const char* path = nullptr; // nullptr = no replay plays
		std::string picked;
		int picked_pov = -1; // the replay list's point of view, -1 = none
		bool listed = false; // path is the replay list's pick
	};
	extern PlaySource s_play_src;
	struct PlayCommon
	{
		bool on = false; // from PlayLoad or PlayCommonArm up to the arm (PlayCommonStart)
		std::vector<std::vector<u8>> answers; // the file's lobby answers (RbkBody)
		LimiterModeType rearm_limiter = LimiterModeType::Nominal; // the limiter before s_play.rearm_seek
	};
	extern PlayCommon s_play_common;
	extern s32 g_frame_period_trim_us;
	// read at VM start (GgpoOnVmInitialize)
	struct GgpoConfig
	{
		std::string options;
		bool net = false; // net=1 in options, or a replay plays
		int lobby_port = 7001; // setting DEV9/Eth ZdxsvGgpoPort (0: a random port, saved); port= overrides it
		int min_delay = 2; // mindelay=, else setting DEV9/Eth ZdxsvGgpoMinDelay
	};
	extern GgpoConfig s_config;
	extern bool g_ggpo_active;
	extern bool g_ggpo_in_rollback;
	constexpr u32 BUTTONS = 16; // PadDualshock2::Inputs PAD_UP .. PAD_R3

	struct Input
	{
		u16 buttons; // bit i = PadDualshock2::Inputs i held
		u8 lx, ly, rx, ry;
		u8 unused[2];
	};
	static_assert(sizeof(Input) == 8);

	// GGPO input of one frame: the host pad's (A, B) plus the kind-3 msgs the game sent K3_LAG frames
	// earlier. GGPO player = battle position + 1.
	struct NetInput
	{
		Input pad; // that player's pad 0
		u8 seq; // +1 per input with new msgs; unchanged input = nothing new (GGPO predicts this)
		u8 len;
		u8 data[22]; // msgs in [0, len); with len <= DESYNC_TRAILER_AT, the desync trailer at DESYNC_TRAILER_AT
	};
	static_assert(sizeof(NetInput) == 32);
	// Desync trailer (GgpoDesync.cpp): u8 tag (sampled frame / DESYNC_PERIOD % 255 + 1, 0 = none), low 24 bits of that
	// frame's ReplayStateHash, LE.
	constexpr int DESYNC_PERIOD = 16;
	constexpr u8 DESYNC_TRAILER_AT = sizeof(NetInput::data) - 4;
	struct DesyncState
	{
		int frame[64] = {}; // sampled frame + 1 (0 = none) per (frame / DESYNC_PERIOD) & 63
		u32 hash[64] = {};
		int checked[GGPO_MAX_PLAYERS] = {-1, -1, -1, -1}; // per position: the last sample compared
		u8 checked_tag[GGPO_MAX_PLAYERS] = {}; // per position: the trailer tag of that sample
		int first[GGPO_MAX_PLAYERS] = {-1, -1, -1, -1}; // per position: the first sample that differed, -1 = none
		u32 checks = 0, mismatches = 0;
	};
	extern DesyncState s_desync;
	void DesyncSample(int f);
	void DesyncClaim(NetInput& in);
	void DesyncCheck(const NetInput* in, int f);
	void DesyncReport();
	std::string DesyncReportLines();
	// ZDXSV_RBK=i/N; also set by a replay's common start (PlayLoad); back to ZDXSV_RBK at VM shutdown
	struct RbkRole
	{
		bool on = false;
		int me = -1, n = 0;
	};
	extern RbkRole s_rbk;
	extern const char* s_rand_env;
	extern const bool s_pw_hash;
	constexpr u32 PW_BASE = 0x8395d8, PW_SIZE = 0x2200;
	extern std::map<int, std::array<u64, 5>> s_pw;
	// Player position (3 floats in the player work) and the 2 game RNGs (u16), hashed by synctest hash=pos.
	constexpr u32 PW_POS = 0x2a8, RNG_A = 0x6d7940, RNG_B = 0x6d793c;
	// The MS asset slot cache: one s8 MS id per slot, -1 = empty. It holds the MSes this machine loaded before, and the
	// battle load sets player work +0x2067 from it.
	constexpr u32 MS_SLOTS = 0x6f2b6c, MS_SLOTS_N = 6;
	void MsSlotsClear();

	// Lobby game patches (ServerPatches.cpp): Begin checks each patch's game and originals and writes the matching
	// ones, Frame rewrites the every-frame ones, End writes the originals back.
	void ServerPatchesBegin(const std::vector<GamePatch>& patches);
	void ServerPatchesFrame();
	void ServerPatchesEnd();
	// replay.proto BattleLogFile patches: Put writes one field per patch, Read takes one (false: malformed or out of range).
	void ServerPatchesPut(std::vector<uint8_t>& pb, uint32_t field, const std::vector<GamePatch>& patches);
	bool ServerPatchRead(const uint8_t* b, size_t n, GamePatch& out);
	constexpr int REPLAY_FILE_VERSION = 20261121; // BattleLogFile.log_file_version of the files written here
	// As files of log_file_version `version` hashed the state
	u32 ReplayStateHash(s64 version = REPLAY_FILE_VERSION);
	u32 GameRng();
	static constexpr u32 TICK_STATE = 0xc627b4; // u8 game phase; 8 = battle load
	static constexpr u8 TICK_LOAD = 8, TICK_PLAY = 6, TICK_END = 7; // 7 = round or game end phase
	u32 RoundRecord();
	u32 MsIds();
	int RoundResult(u32 a, u32 b);
	extern std::FILE* s_pw_dump;
	// Play-start barrier: the n-th battle load passes once every peer's synced count (Input::unused[1])
	// reaches n. The MS-select load has the same barrier (count in Input::unused[0]), off when a file
	// recorded without it plays. Part of the rollback state.
	struct PS
	{
		u8 n, rel;
		bool hold, go;
		bool operator==(const PS&) const = default;
	};
	extern bool s_ms_on;
	extern const int s_k3_lag;
	struct NetOut
	{
		int frame; // GGPO frame of the send
		size_t idx; // index in s_battle.sent
		std::vector<u8> m;
	};
	// The HLE state outside the VM that a rollback (s_net_at) or a replay key (PlayKey) restores with the
	// VM state. A new field here is saved and restored everywhere.
	struct RollbackState
	{
		size_t net_pos = 0; // msgs sent so far in the current timeline
		int end_frame = -1; // frame that sent the end msg (kind f) in the current timeline, -1 = not yet
		std::vector<u8> net_rx; // remote msgs not yet given to the game's recv
		int zds_seen[GGPO_MAX_PLAYERS] = {}, zds_rel = 0; // zds kind-3 barrier counters
		PS ps = {}, ms = {};
	};
	extern RollbackState s_rb;
	// Per-frame rings that a rollback leaves in place (they hold every frame it can go back to) and a
	// replay key copies whole.
	struct FrameRings
	{
		Input zd_pad[128] = {}; // own host pad per frame & 127 (reruns reapply it)
		u16 zd_hist[128][GGPO_MAX_PLAYERS][2] = {}; // synced (A, B) per frame & 127
		u8 net_seq_at[64][GGPO_MAX_PLAYERS] = {}; // per frame & 63: each player's synced seq
	};
	extern FrameRings s_rings;
	// The state at a battle load end that a later load end's state needs beyond the first one's (replay.proto
	// round_states): EE RAM ranges (concatenated in a fixed order) and the HLE state. The rings and the kind-3
	// bodies are rebuilt from the inputs before it.
	struct RoundState
	{
		int frame = -1;
		std::vector<u8> ee;
		RollbackState rb;
	};
	RoundState RoundStateTake(int f);
	std::vector<uint8_t> RoundStatePb(const RoundState& s);
	bool RoundStateRead(const uint8_t* b, size_t n, RoundState& out);
	// The file's inputs before s.frame rebuild the rings and kind-3 bodies; false: s does not fit (nothing written).
	bool RoundStateApply(const RoundState& s, const std::vector<NetInput>& inputs);
	// The battle socket's HLE state of the current battle.
	struct BattleNet
	{
		bool started = false; // the session was opened (lobby=1: until the next battle's NetReset)
		bool armed = false, over = false, closed = false;
		bool local_server = false; // lobby=1: the battle server was answered on the EE (BattleServerCall)
		bool lobby_cut = false; // lobby=1 ping test failed: the battle connection is silent (LobbyCutCall)
		u32 cut_sends = 0;
		int me = -1; // local battle position
		std::vector<std::vector<u8>> sent; // every msg the game sent since armed, in order
		std::vector<int> sent_at; // GGPO frame of each sent entry (latest timeline)
		std::deque<NetOut> out; // committed, not yet in a local input
		NetInput local = {};
		int frame = 0; // GGPO frame being run (last save or load)
		int end = -1; // frames since the end msg (kind f) was sent or received, -1 = not yet
		int last_real = INT_MAX; // last frame with the peers' inputs (the session left them at the end), INT_MAX = none
		u32 zd_steps = 0, zd_changed = 0;
		// Live steps per GGPO frame and position. Gaps (frames without a step) come with scene changes.
		int zd_last[GGPO_MAX_PLAYERS] = {-1, -1, -1, -1};
		u32 zd_repeats = 0, zd_gaps = 0;
		int zd_gap_min = INT_MAX, zd_gap_max = 0;
		u32 zds_echo = 0, zds_skip = 0;
		// zds: kind 3 (round handshake) is the one barrier: each machine reaches it at its own frame
		// (scene/load timing: one side can be a frame later), so it goes through the GGPO input
		// and the n-th kind 3 of every remote goes to recv once all peers' n-th is in the synced stream.
		std::vector<std::vector<u8>> zds_k3[GGPO_MAX_PLAYERS]; // per sender, by index
		u32 zds_k3rel = 0;
	};
	extern BattleNet s_battle;
	struct NetStats
	{
		u32 sends, msgs, recvs, rxmsgs, rxbytes, polls, other, nowait, senddiff, toolong, maxq, restamp, late;
	};
	extern NetStats s_ns;

	struct SessionStat
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

	struct RunState
	{
		bool running = false; // net: GGPO_EVENTCODE_RUNNING seen
		bool disconnected = false;
		int disconnected_until = -1; // net: last frame run after a peer left before the end msg
		int frames_ahead = 0; // net: last GGPO_EVENTCODE_TIMESYNC
		int waits = 0; // net: frames that waited for a peer
		int sync_frames = 0; // net: frames slept for GGPO time sync
		double sync_max_ms = 0;
		int save_skipped = 0;
		int top_frame = 0; // the frame of the last save outside a rollback
		int reruns_left = 0; // rerun frames left in this rollback
		// LowLatencyPresent: present, rollback, then sleep and poll input (GgpoDeferThrottle). A corrected frame
		// shows one frame later.
		bool throttle_deferred = false;
		// GGPO ignores the load callback's result and reruns from whatever state is loaded.
		bool load_failed = false;
		Common::Timer::Value barrier_since = 0; // net: wall time a load barrier started holding, 0 = none
	};

	struct SessionStats
	{
		int rollback_frames = 0, loads = 0, mismatches = 0, ggpo_warnings = 0;
		SessionStat save_ms, hash_ms, load_ms;
		SessionStat rerun_ms, wait_ms; // between frames: rollback rerun emulation, net wait for a peer
		SessionStat sleep_ms; // between frames: limiter sleep and input poll deferred by GgpoDeferThrottle
		SessionStat rerun_mcycles; // EE cycles (millions) per rerun frame
		SessionStat emu_ms, exit_ms, ours_ms; // wall: last GgpoOnExecuteReturned end -> GgpoOnVsync -> GgpoOnExecuteReturned start -> its end
		Common::Timer::Value t_vsync = 0, t_returned = 0;
		// Output of the session: GS frames presented (g_perfmon, GS thread; read here for the report only)
		// since the start, SPU2 samples played and dropped in rerun frames.
		int gs_frame0 = 0;
		s64 spu_played = 0, spu_dropped = 0;
	};
	extern RunState s_run;
	extern SessionStats s_stats;
	extern GGPOPlayerHandle s_handles[GGPO_MAX_PLAYERS];

	// GgpoNetStatus.cpp
	void NetStatusSetPeer(GGPOPlayerHandle h, int state); // 0 connected, 1 interrupted, 2 disconnected
	void NetStatusSamplePings();
	void NetStatusSampleFps();
	std::string NetStatusReport();
	void NetStatusUpdate(bool log);
	void NetStatusHold(bool disconnected);
	void NetStatusPeersConnected();
	void NetStatusReset();
	enum class Hash
	{
		Pw,
		Pos,
		Full
	};
	// ZDXSV_GGPO key=value options, set by Parse; players, delay and net also change at runtime.
	struct GgpoOptions
	{
		bool net = false;
		int players = 4;
		Hash hash = Hash::Pw; // synctest checksum
		int port = 7001;
		int delay = 0;
		bool delay_set = false; // delay= given: fixed; else a lobby battle picks it from the peers' rtt
		bool lobby = false;
		bool bad_session = false; // badsession=1 (test): the ping test uses another session id
		std::string replay_dir; // replay=DIR; without it lobby=1 saves to <data dir>/replays, other net runs none
		bool replay_off = false; // replay=0
		std::string upload_url; // upload=URL (test), else setting DEV9/Eth ZdxsvReplayUploadUrl
		std::string live_url; // live=URL (test): the lobby API for the live uplink, else setting DEV9/Eth ZdxsvLobbyApiUrl
		int relay = 0; // relay=R (net): remote p is at port R + 8 * me + p (one UDP relay per pair), not port + p
		int start = 1500, frames = 3000, check = 6;
		u32 seed = 1;
		bool host_input = false, no_input = false, control_input = false;
		u16 mask = 0xffff;
		bool sync = true; // sync=0: no state hashes (checksum 0)
		std::string peer_host = "127.0.0.1";
		bool osd = true;
		bool frame_osd = false; // setting DEV9/Eth ZdxsvFrameOsd
	};
	extern GgpoOptions s_opt;
	// lobby=1: the last battle info (SetLobbyPeers, CPU thread), guarded by s_lobby_mtx
	struct LobbyBattle
	{
		bool received = false, logged = false, unreachable = false;
		int me = -1; // our position, -1 = not listed
		std::vector<std::vector<Zdxsv::PeerAddr>> peers; // candidates per position
		std::vector<Zdxsv::BattleInfo::Relay> relays;
		u32 session = 0; // ggpo_session
		u32 gen = 0; // battle infos received
		std::vector<LobbyPlayer> players; // user id, name + pilot name per position
		std::vector<GamePatch> patches;
		std::string ids; // battle info lines naming the battle (SetLobbyPeers)
	};
	// The battle GGPO armed with, taken from LobbyBattle.
	struct ArmedBattle
	{
		std::vector<Zdxsv::PeerAddr> peers; // per position, picked when armed
		std::vector<int> via; // per position: PingResult.via (0 direct, 1 peer relay, 2 relay server)
		std::vector<Zdxsv::RelayServerAddr> servers; // relay servers, registered with GGPO in order
		std::vector<LobbyPlayer> players;
		std::vector<GamePatch> patches;
		u32 gen = 0; // the LobbyBattle gen armed with
	};
	extern LobbyBattle s_lobby;
	extern ArmedBattle s_net;
	extern std::vector<GamePatch> s_battle_patches; // the started battle's, for its replay header
	extern std::mutex s_lobby_mtx;
	extern std::string s_report; // P2PMatchingReport of the last lobby battle (TakeLobbyReport)
	extern GGPOSession* s_session;
	extern bool s_frame_ended; // the CPU left Execute() at a vsync
	extern int s_session_frames;
	extern std::array<float, PadDualshock2::Inputs::LENGTH> s_host;
	Input TakeoverHostInput();
	void ApplyPad(int p, const Input& in);

	// First-run save of a frame, to name what a rerun changed.
	struct Sample
	{
		std::vector<u64> pages; // hash per EE RAM page
		std::vector<u8> state; // the rest
	};
	constexpr u32 PAGE_SIZE = 4096;
	extern std::map<int, Sample> s_first;
	extern bool s_rerun; // the save is of a rerun frame
	extern int s_diff_logged;
	void DiffRaw(int frame, const std::vector<u8>& first, const std::vector<u8>& raw);
	void Diff(int frame, const Sample& first, const std::vector<u64>& pages, const std::vector<u8>& state);
	void HashSave(int frame, int* checksum);
	void RerunKeepBeforeLoad(bool callbacks_skipped);
	void RerunKeepAfterLoad();
	void RerunKeepAfterRerunFrame(bool last);
	bool RerunKeepWalksObjects(); // a skipped rerun frame runs the object draw walker (OnDrawRun)
	void RerunKeepSeekFrame(bool skipped); // a replay seek frame: skipped ones walk the parts as rerun frames do
	bool RerunKeepDrawsHandler(u32 handler, u32 obj); // the walker of a skipped rerun frame calls this handler
	extern bool s_replay_rec; // a battle is being recorded
	std::string ReplayIds();
	std::string IdValue(const std::string& ids, std::string_view key);
	void ReplayBegin();
	void ReplayLog(int f, const NetInput* in);
	void ReplayWrite(const char* what);
	std::string ReplayRoundsReport(); // "win:ms,..;.." per round, ms = the MS id + 1 by position
	void NetReset();
	bool LobbyNewBattle();
	bool LobbyPendingBattle(int& me, int& n);
	constexpr u32 NET_FNO_SEND = 0x10;
	constexpr u32 NET_FNO_CONNECT = 7;
	constexpr u32 NET_FNO_CLOSE = 0xd;
	constexpr u32 NET_BATTLE_SOCK = 0;
	extern u32 s_game_gp; // game gp, latched in OnNetRpc
	extern std::FILE* s_net_trace;
	void ZdPadAB(const Input& in, u16& a, u16& b);

	// One key slot per frame: counter c (6 bits), game-wide k and X (X only in records), and for an
	// input record its A/B words. Key msg (kind 2) = 2 slots: `(0x80|c, k)` or `00 c A0 A1 X k B0 B1`.
	struct KeySlot
	{
	u8 c, x, k;
	bool rec;
	u16 a, b;
	};
	bool ParseKeySlots(const u8* m, u32 n, std::vector<KeySlot>& out);
	void TracePad();
	void TraceInputs();
	constexpr u32 NET_RES_LEN = 0xc22c98;
	void NoteOwnSend(const KeySlot& s);
	constexpr u32 NET_FNO_POLL = 0xf;
	constexpr u32 NET_FNO_RECV = 0x14;
	void NetSaved(int frame);
	Input RandInput();
	NetInput NetPack(const Input& pad, int frame);
	void NetApply(const NetInput* in);
	void NetRelease(const NetInput* in);
	extern const int s_net_barrier_ms;
	bool NetBarrierTimedOut();
	void NetReport();
	// One file's play state; PlayReset clears it whole, so only state that starts fresh per file belongs here.
	struct PlayState
	{
		std::vector<NetInput> inputs;
		std::vector<u32> hashes;
		std::map<int, u32> rngs; // frame -> GameRng
		int rng_pos = -1;
		std::vector<std::pair<int, int>> cut;
		std::vector<u8> ps_rel; // per frame: the file's play-start barrier releases (PS::rel) before it
		std::vector<u8> ms_rel; // the same for the MS-select barrier
		int frames = 0;
		int rearm_seek = -1; // a switch's common start: the frame to seek to from its key 0
		bool booted_saved = false; // ZDXSV_REPLAY_STATE empty: the booted state is in the cache (PlayCommonState)
		std::deque<std::pair<int, int>> seeks; // ZDXSV_REPLAY_SEEK
		bool at_end = false; // paused after the last frame (no ZDXSV_REPLAY_EXIT)
		int target = -1; // seeking: frames run unlimited up to this one
		int seek_from = -1;
		int then = -1; // seeking to the first load end to save its key: the round jump's target after it
		bool then_req = false; // the seek request is that target: not limited to the round it was run from
		u64 seek_t0 = 0;
		int hi = -1, tick_f = -1;
		int run_load = -1; // running unlimited until this load has ended, -1 = none (skip MS selection = 1)
		// Per game, the frames shown with skipping on: [the briefing's load end, the rematch screen's load end).
		std::vector<std::pair<int, int>> spans;
		std::vector<int> plays; // per span: the file's play start
		bool step_pause = false; // pause again at the seek's target
		int speed = 0; // -2..2
		std::deque<std::pair<int, int>> round_at; // ZDXSV_REPLAY_ROUND_AT=frame:round,...
		std::deque<std::pair<int, int>> pov_at; // ZDXSV_REPLAY_POV_AT=frame:position,...
		bool pov_ok[GGPO_MAX_PLAYERS] = {};
		std::vector<GamePatch> patches; // the file's, applied from each common start
		std::map<int, RoundState> round_states; // the file's, by frame (a round jump writes one over the key of the first)
		s64 version = 0; // the file's log_file_version
		bool states_off = false; // a round jump differed: no more round states
		int jump_check = -1; // after a round jump to this frame: state hashes are not checked up to the next load end
		std::set<int> select_starts; // the file's MS-select load ends (the first load end and the one before each span)
		std::vector<int> loads; // the file's load ends, in order (as s_play_loads.battle)
	};
	extern PlayState s_play;
	// Written on the CPU thread; the GS thread reads it under s_battle_loads_mtx.
	struct PlayLoads
	{
		std::vector<int> battle;
		std::vector<int> file_rounds, results;
		std::vector<u32> file_ms, ms; // per round, as MsIds packs them
	};
	extern PlayLoads s_play_loads;

	// Seek key: the full state as .p2s in the cache folder (zipped on a thread) plus the HLE state outside it.
	// A seek loads the newest key at or before the target and runs up to it unlimited.
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
	extern std::map<int, std::unique_ptr<PlayKey>> s_play_keys[GGPO_MAX_PLAYERS]; // by point of view (position)
	extern std::atomic<int> s_play_req; // requested seek target, INT_MIN = none
	extern bool s_play_skip_ms;
	extern std::atomic<bool> s_keys_on; // replay key display
	extern std::mutex s_battle_loads_mtx;
	extern std::atomic<int> s_play_round_req; // requested round, 0 = briefing, INT_MIN = none (with spans: 1 = the first span)
	void PlayKeyApply(const PlayKey& k);
	int PlaySeek(int target, bool pov_switch = false);
	// The span holding frame f, else the next one, else the last; -1 = no spans or skipping off.
	int PlaySpanAt(int f);
	// Spectating live past the last shown round (between rounds): the next round, not started yet, is shown.
	bool PlayLiveNextRound(int f);
	int PlaySkipGap(int next);
	bool PlayInBattle(int f);
	void ReplayPadPoll();
	void ReplayControlsReset();
	void PlayHideAfterLoad();
	int PlayFastEvery();
	// Resume from a pause for a step or a seek that pauses again (s_play.step_pause): the audio output stays paused.
	void PlayStepResume();
	// Logs the step time; false: a queued step has started instead of the pause.
	bool PlayStepPaused(int f);
	extern std::atomic<int> s_play_step_queue; // step presses made while a step ran, + forward / - back
	void PlayStepRingSave(int f);
	int PlayStepRingLoad(int target);
	void PlayStepRingClear();
	void ReplayBenchStart();
	void ReplayBenchSave(int f);
	void ReplayBenchNext(int next);
	void ReplayBenchReset();
	void PlaySlowWait();
	// Not refused by ReplayRefuses.
	bool ReplayLoadState(const char* path, Error* error);
	void PlayRunBegin(int load);
	void PlayRunEnd(const char* why, int f);

	// Takeover: from frame T the own position plays the host pad, packed as in a live battle (NetPack) with
	// mindelay= frames of input delay; the other positions keep the file's inputs.
	enum : int { TO_OFF, TO_ALIGN, TO_COUNT, TO_ON };
	enum : int { TO_REQ_NONE, TO_REQ_TAKE, TO_REQ_START, TO_REQ_RETRY, TO_REQ_RETURN, TO_REQ_CANCEL };
	// Read and written by the UI thread too; reset field by field (Takeover is reset whole).
	struct TakeoverShared
	{
		std::atomic<int> phase{TO_OFF};
		std::atomic<int> req{TO_REQ_NONE};
		std::atomic<u16> target{0}; // own B of the file at T
	};
	extern TakeoverShared s_to_shared;
	struct Takeover
	{
		int frame = -1; // T
		int delay = 0;
		bool start_held = false;
		int pause_in = 0;
		std::unique_ptr<PlayKey> key; // the state before T
		std::vector<std::vector<u8>> sent; // own sends before T
		std::vector<int> sent_at;
		std::deque<NetOut> out; // own kind-3 msgs before T not in the file's own inputs up to T + delay - 1
		NetInput local; // the file's own input at T + delay - 1
		std::deque<NetInput> queue; // packed own inputs, `delay` frames ahead
		Common::Timer::Value count_t0 = 0;
	};
	extern Takeover s_to;
	extern const char* const s_to_test;
	extern int s_to_test_at;
	extern int s_to_test_retry;
	bool TakeoverBegin(int t);
	int TakeoverLoad();
	void TakeoverStart();
	void TakeoverOff(const char* why);
	void PlayFrame(int f);
	bool PlayLoadWait(int f);
	void PlayReport(const char* what);
	void PlayStop(const char* what);
	extern std::atomic<int> s_play_pov_req; // requested position, -1 = none
	struct PlaySent
	{
		std::vector<std::vector<u8>> sent;
		std::vector<int> at;
		std::deque<NetOut> out;
	};
	extern PlaySent s_play_sent[GGPO_MAX_PLAYERS];
	extern std::unique_ptr<Zdxsv::LiveDown> s_live_down;
	struct LivePlay
	{
		Zdxsv::LiveStreams got; // inputs not in s_play.inputs yet (part of a frame)
		std::string close; // "" = running
		bool catchup = false, close_logged = false;
		bool behind = false; // a seek or round jump of the viewer: no catch-up until playback is at the newest frames again
		int waits = 0;
		int hold_want = -1; // the frame count the hold at the newest frame waits for, -1 = none
		Common::Timer hold_wait;
		double wait_ms = 0;
	};
	extern LivePlay s_live_play;

	// Live auto-next: after a closed stream, a thread polls the lobby for the newest battle not watched yet.
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
	extern std::unique_ptr<LiveWait> s_live_wait;
	// Live auto-next: kept across the resets.
	struct LiveNextState
	{
		std::vector<std::string> seen; // battle codes watched, oldest first
		int left = -1; // battles left to move on to, -1 = the setting decides (no limit)
		int skips = 0; // battles auto-next could not play
		std::string url; // the battle auto-next moved on to, "" = ZDXSV_REPLAY
	};
	extern LiveNextState s_live_next;
	std::optional<std::vector<u8>> LiveOpen(const std::string& url);
	// A live stream's round frames (and with `states` its round states) as the file's: spans, MS-select starts, round states.
	void PlayLiveRoundFrames(const std::vector<u8>& round_frames, const std::vector<u8>& states, bool with_states);
	void PlayFileRels(u8 ps0, u8 ms0);
	bool LiveAutoNext(bool skip = false);
	void LiveCatchupEnd(int f);
	void LivePaceReset();
	// Returned, before PlayNext: true = hold the frame, the newest one is played and the stream is open; until
	// LIVE_BUFFER more frames, the stream's close or a seek.
	bool LiveHold();
	int LiveNext(int next); // the frame to run next
	extern bool s_sync_chase; // s_play_req is a chase, not a host seek for the guests
	void SyncStop();
	void SyncStart(int me);
	void SyncFollow(int next);
	void SyncFrame(int next);
	extern u32 s_play_gen; // bumped by PlayReset: a call queued before it compares it and drops out
	void PlayReset();
	void PlayKeysPrune(); // deletes the cache folder's seek keys a killed or crashed replay left
	void PlayLoad();
	void PlayFetchStop(); // cancels and joins the replay downloads
	bool PlayCommonArm(int pov, int seek = -1);
	void PlayCommonStart();
	void PlayBegin(int me);
	void PlayNext();
	void RbkReset();
	bool LobbyArm(int me);
	void LobbyReset();
	bool LobbyCutCall(u32 fno, s16 sock, s16 len);
	float GgpoBudgetLearned();
	float GgpoBudgetUsed();
	int GgpoBudgetDelay(int rtt, int min_delay, float budget);
	void GgpoBudgetFrame(bool sample, double rerun_ms, double wait_ms, double sleep_ms);
	void GgpoBudgetSessionEnd(bool learn);

	// GgpoFrameOsd.cpp
	void GgpoFrameOsdSample(int frame);
	void GgpoFrameOsdReset();

	// Ggpo.cpp
	constexpr int GGPO_PLAYERS = 2;
	extern RollbackState s_net_at[128];
	extern bool s_vm_closing;
	extern std::mt19937 s_ggpo_rng;
	void GgpoPriority(bool high);
	void GgpoReport(const char* what);
	Input GgpoHostInput();
	bool GgpoSyncAndApply(bool rerun);
	bool GgpoRunFrame();

	// GgpoSession.cpp
	void GgpoStop(const char* what);
	const char* GgpoStart();
	void NetEndCheck();
	bool NetAddLocalInput(int frame);
	bool NetWaitPredictionLimit();
	bool NetSyncAndApply();
} // namespace Zdxsv
