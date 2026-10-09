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
	extern const char* const s_play_env;
	extern bool s_play_common;
	extern s32 g_frame_period_trim_us;
	extern std::string s_options;
	// The hosted post-entry save state a replay or live battle starts from (PlayCommonState)
	constexpr const char* REPLAY_STATE_URL = "https://storage.googleapis.com/zdxsv/misc/rbk-p1.p2s";
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
	extern bool s_net_env; // net=1 in s_options, or a replay plays (GgpoOnVmInitialize)
	extern int s_rbk_me, s_rbk_n;
	extern bool s_rbk; // also set by a replay's common start (PlayLoad)
	extern const char* s_rand_env;
	extern bool s_net; // net=1 parsed
	extern int s_players; // players= (net)
	extern u32 s_zds_echo, s_zds_skip;
	extern const bool s_pw_hash;
	constexpr u32 PW_BASE = 0x8395d8, PW_SIZE = 0x2200;
	extern std::map<int, std::array<u64, 5>> s_pw;
	// Synctest hash=pos: x, y, z (3 floats at player work + 0x2a8) of the 4
	// players, then u16 0x6d7940 and u16 0x6d793c (the game RNGs, generators 0x20f4b0 / 0x20f4f0).
	constexpr u32 PW_POS = 0x2a8, RNG_A = 0x6d7940, RNG_B = 0x6d793c;
	u32 ReplayStateHash();
	u32 GameRng();
	static constexpr u32 TICK_STATE = 0xc627b4; // u8 game phase; 8 = battle load
	static constexpr u8 TICK_LOAD = 8, TICK_PLAY = 6, TICK_END = 7; // 7 = round or game end phase
	u32 RoundRecord();
	int RoundResult(u32 a, u32 b);
	extern std::FILE* s_pw_dump;
	extern std::vector<std::vector<u8>> s_zds_k3[GGPO_MAX_PLAYERS]; // per sender, by index
	extern u32 s_zds_k3rel;
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
	extern u32 s_zd_steps, s_zd_changed;
	extern bool s_net_armed, s_net_over;
	extern bool s_lobby_cut; // lobby=1 ping test failed: the battle connection is silent (LobbyCutCall)
	extern u32 s_cut_sends;
	extern int s_net_me; // local battle position
	extern std::vector<std::vector<u8>> s_net_sent; // every msg the game sent since armed, in order
	extern const int s_k3_lag;
	struct NetOut
	{
		int frame; // GGPO frame of the send
		size_t idx; // index in s_net_sent
		std::vector<u8> m;
	};
	extern std::deque<NetOut> s_net_out; // committed, not yet in a local input
	extern std::vector<int> s_net_sent_at; // GGPO frame of each s_net_sent entry (latest timeline)
	extern NetInput s_net_local;
	extern int s_net_frame; // GGPO frame being run (last save or load)
	// The HLE state outside the VM that a rollback (s_net_at) or a replay key (PlayKey) restores with the
	// VM state. A new field here is saved and restored everywhere.
	struct RollbackState
	{
		size_t net_pos = 0; // msgs sent so far in the current timeline
		std::vector<u8> net_rx; // remote msgs not yet given to the game's recv
		int zds_seen[GGPO_MAX_PLAYERS] = {}, zds_rel = 0; // zds kind-3 barrier counters
		PS ps = {};
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
	extern int s_net_end; // frames since the end msg (kind f) was sent or received, -1 = not yet
	struct NetStats
	{
		u32 sends, msgs, recvs, rxmsgs, rxbytes, polls, other, nowait, senddiff, toolong, maxq, restamp, late;
	};
	extern NetStats s_ns;
	enum class Hash
	{
		Pw,
		Pos,
		Full
	};
	extern Hash s_hash;
	extern int s_port, s_delay;
	extern bool s_delay_set; // delay= given: fixed; else a lobby battle picks it from the peers' rtt
	extern int s_min_delay; // mindelay=, else setting DEV9/Eth ZdxsvGgpoMinDelay
	extern bool s_lobby; // lobby=1
	extern bool s_bad_session; // badsession=1 (test): the ping test uses another session id
	extern std::string s_replay_dir; // replay=DIR; without it lobby=1 saves to <data dir>/replays, other net runs none
	extern bool s_replay_off; // replay=0
	extern std::string s_upload_url; // upload=URL (test), else setting DEV9/Eth ZdxsvReplayUploadUrl
	extern std::vector<LobbyPlayer> s_lobby_players, s_net_players;
	extern std::mutex s_lobby_mtx;
	extern std::vector<Zdxsv::PeerAddr> s_net_peers; // per position, picked when armed
	extern std::vector<int> s_net_via; // per position: PingResult.via (0 direct, 1 peer relay, 2 relay server)
	extern std::vector<Zdxsv::RelayServerAddr> s_net_servers; // relay servers, registered with GGPO in order
	extern u32 s_lobby_session; // ggpo_session
	extern std::string s_report_ids; // battle info lines naming the battle (SetLobbyPeers)
	extern std::string s_report; // P2PMatchingReport of the last lobby battle (TakeLobbyReport)
	extern GGPOSession* s_session;
	extern bool s_started; // the session was opened (lobby=1: until the next battle's NetReset)
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
	extern bool s_replay_rec; // a battle is being recorded
	std::string ReplayIds();
	std::string IdValue(const std::string& ids, std::string_view key);
	void ReplayBegin();
	void ReplayLog(int f, const NetInput* in);
	void ReplayWrite(const char* what);
	void NetReset();
	bool LobbyNewBattle();
	constexpr u32 NET_FNO_SEND = 0x10;
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
	NetInput NetPack(const Input& pad);
	void NetApply(const NetInput* in);
	void NetReport();
	extern std::vector<NetInput> s_play_inputs;
	extern std::vector<u32> s_play_hashes;
	extern std::map<int, u32> s_play_rngs; // frame -> GameRng
	extern int s_play_rng_pos;
	extern std::vector<std::pair<int, int>> s_play_cut;
	extern int s_play_frames;
	extern std::vector<std::vector<u8>> s_play_answers; // common start: the file's lobby answers (RbkBody)
	extern int s_play_rearm_seek; // a switch's common start: the frame to seek to from its key 0
	extern LimiterModeType s_play_rearm_limiter; // and the limiter before it
	extern bool s_play_booted_saved; // ZDXSV_REPLAY_STATE empty: the booted state is in the cache (PlayCommonState)

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
	extern std::map<int, std::unique_ptr<PlayKey>> s_play_keys[GGPO_MAX_PLAYERS]; // by point of view (position)
	extern std::deque<std::pair<int, int>> s_play_seeks; // ZDXSV_REPLAY_SEEK
	extern std::atomic<int> s_play_req; // requested seek target, INT_MIN = none
	extern bool s_play_at_end; // paused after the last frame (no ZDXSV_REPLAY_EXIT)
	extern int s_play_target; // seeking: frames run unlimited up to this one
	extern LimiterModeType s_play_limiter;
	extern const bool s_play_skip_ms;
	extern std::mutex s_battle_loads_mtx;
	extern std::vector<int> s_battle_loads; // written on the CPU thread; the GS thread reads it under s_battle_loads_mtx
	extern int s_play_hi, s_tick_f;
	extern std::vector<int> s_play_file_rounds, s_round_results;
	extern int s_run_load; // running unlimited until this load has ended, -1 = none (skip MS selection = 1)
	extern std::atomic<int> s_play_round_req; // requested round, 0 = briefing, INT_MIN = none
	extern std::deque<std::pair<int, int>> s_play_round_at; // ZDXSV_REPLAY_ROUND_AT=frame:round,...
	extern bool s_play_pov_ok[GGPO_MAX_PLAYERS];
	void PlayKeyApply(const PlayKey& k);
	int PlaySeek(int target, bool pov_switch = false);
	void PlayRunBegin(int load);
	void PlayRunEnd(const char* why, int f);

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
	extern std::atomic<int> s_to_phase;
	extern std::atomic<int> s_to_req;
	extern std::atomic<u16> s_to_target; // own B of the file at T
	extern int s_to_frame; // T
	extern bool s_to_skip, s_to_start_held;
	extern const char* const s_to_test;
	extern int s_to_test_at;
	extern int s_to_test_retry;
	bool TakeoverBegin(int t);
	int TakeoverLoad();
	void TakeoverStart();
	void TakeoverOff(const char* why);
	void PlayFrame(int f);
	void PlayReport(const char* what);
	void PlayStop(const char* what);
	extern std::atomic<int> s_play_pov_req; // requested position, -1 = none
	extern std::deque<std::pair<int, int>> s_play_pov_at; // ZDXSV_REPLAY_POV_AT=frame:position,...
	struct PlaySent
	{
		std::vector<std::vector<u8>> sent;
		std::vector<int> at;
		std::deque<NetOut> out;
	};
	extern PlaySent s_play_sent[GGPO_MAX_PLAYERS];
	extern std::unique_ptr<Zdxsv::LiveDown> s_live_down;
	extern Zdxsv::LiveStreams s_live_got; // inputs not in s_play_inputs yet (part of a frame)
	extern std::string s_live_close; // "" = running
	extern bool s_live_catchup, s_live_close_logged;
	extern int s_live_waits;
	extern double s_live_wait_ms;

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
	extern std::unique_ptr<LiveWait> s_live_wait;
	extern std::string s_live_next_url; // the battle auto-next moved on to, "" = ZDXSV_REPLAY
	std::optional<std::vector<u8>> LiveOpen(const std::string& url);
	bool LiveAutoNext();
	void LiveCatchupEnd(int f);
	void LivePaceReset();
	void LiveNext(int next);
	extern bool s_sync_chase; // s_play_req is a chase, not a host seek for the guests
	void SyncStop();
	void SyncStart(int me);
	void SyncFollow(int next);
	void SyncFrame(int next);
	void PlayReset();
	void PlayLoad();
	bool PlayCommonArm(int pov, int seek = -1);
	void PlayCommonStart();
	void PlayBegin(int me);
	void PlayNext();
	void RbkReset();
	bool LobbyArm(int me);
	bool LobbyCutCall(u32 fno, s16 sock, s16 len);
} // namespace Zdxsv
