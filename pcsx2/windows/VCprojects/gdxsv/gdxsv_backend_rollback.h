#pragma once
// Mock network implementation to replay local battle log

#include <array>
#include <vector>
#include <queue>
#include <string>
#include <map>

#include "ggpo/src/include/ggponet.h"
#include "gdxsv_ggpo_cb.h"
#include "libs.h"
#include "gdx_rpc.h"
#include "gdxsv.pb.h"
#include "lbs_message.h"
#include "mcs_message.h"


class GdxsvBackendRollback {
public:
	GdxsvBackendRollback(const std::map<std::string, u32>& symbols, std::atomic<int>& maxlag);

	struct GameInput {
		u16 keycode;
	};

	struct GameState {
		std::array<u8, 0x2100 * 4> PlayerWorks;
	};

	enum class State {
		None,
		Start,
		LbsStartBattleFlow,
		McsWaitJoin,
		McsSessionExchange,
		McsInBattle,
		End,
	};

	bool IsReplayTest();

	void Reset();

	bool StartReplayTest(const char* path);

	void Open();

	void Close();

	u32 HandleRPC(const gdx_rpc_t& rpc);

	// ggpo callbacks

	bool ggpo_cb_begin_game(const char*);

	bool ggpo_cb_advance_frame(int);

	bool ggpo_cb_load_game_state(unsigned char* buffer, int len);

	bool ggpo_cb_save_game_state(unsigned char** buffer, int* len, int* checksum, int frame);

	void ggpo_cb_free_buffer(void* buffer);

	bool ggpo_cb_on_event(GGPOEvent* info);

	bool ggpo_cb_log_game_state(char* filename, unsigned char* buffer, int len);

private:
	void SaveCurrentGameState(GameState& state);

	void LoadGameState(const GameState& state);

	u32 OnSockWrite(u32 addr, u32 size);

	u32 OnSockRead(u32 addr, u32 size);

	u32 OnSockPoll();

	void PrepareKeyMsgIndex();

	void ProcessLbsMessage();

	void ProcessMcsMessage();

	void ApplyPatch(bool first_time);

	void RestorePatch();

	const std::map<std::string, u32>& symbols_;
	std::atomic<int>& maxlag_;

	bool is_replay_test_ = false;
	State state_ = State::None;
	LbsMessageReader lbs_tx_reader_{};
	McsMessageReader mcs_tx_reader_{};
	proto::BattleLogFile log_file_{};
	std::deque<u8> recv_buf_{};
	int recv_delay_ = 0;
	int me_ = 0;
	int in_game_scene_ = 0;
	std::vector<McsMessage> msg_list_{};
	std::array<int, 4> start_index_{};
	std::array<int, 4> key_counter_{};
	std::array<std::vector<int>, 4> key_msg_index_{};

	u16 tsw_tmp_ = 0;
	bool is_rollbacking_ = false;
	GGPOSession* ggpo_ = nullptr;
	std::array<GGPOPlayerHandle, 4> ggpo_handle_{};
};
