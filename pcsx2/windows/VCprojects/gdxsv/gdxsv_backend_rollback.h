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

	template<int T_ADDR, int T_SIZE>
	struct GameMemoryRange {
		std::array<u8, T_SIZE> value;

		void Read() {
			gdxsv_ReadMemBlock(value.data(), T_ADDR, T_SIZE);
		}

		void Write() const {
			gdxsv_WriteMemBlock(T_ADDR, value.data(), T_SIZE);
		}
	};

	struct GameState {
		GameMemoryRange<0x0057fd00, 4> CrrView;
		GameMemoryRange<0x0058027c, 8> SwCrnt;
		GameMemoryRange<0x00580348, 4> RandData;
		GameMemoryRange<0x0057fad8, 4> RandomSeed;
		GameMemoryRange<0x0057fadc, 8> KRand;
		GameMemoryRange<0x0057fc00, 0x0057fc44-0x0057fc00> HitWorkWork;
		GameMemoryRange<0x0057fc44, 0x0057fcb0-0x0057fc44> WorkWork;
		GameMemoryRange<0x0072f0e0, 4 * 0x40> WorkMoveLast;
		GameMemoryRange<0x0072f1e0, 4 * 0x40> WorkMoveHead;
		GameMemoryRange<0x0072f2e0, 0x180 * 0x100> HitWork;
		GameMemoryRange<0x007472e0, 0x2c0 * 0x140> Work1;
		GameMemoryRange<0x0077e2e0, 0x200 * 0x140> Work0;
		GameMemoryRange<0x00866620, 0x2100 * 5> PlayerWorks;
		GameMemoryRange<0x00870dd0, 0x0300> SystemWork2;
		GameMemoryRange<0x008710d0, 0x0300> SystemWork;
		GameMemoryRange<0x00aa9610, 4 * 2> InetSys;
		GameMemoryRange<0x00aa8690, 0x00aa86e0-0x00aa8690> McsPsw;
		GameMemoryRange<0x007A90E0, 0x180 * 10> CameraWork;

		void Read() {
			// SwCrnt.Read();
			CrrView.Read();
			RandomSeed.Read();
			RandData.Read();
			WorkMoveLast.Read();
			WorkMoveHead.Read();
			HitWork.Read();
			HitWorkWork.Read();
			KRand.Read();
			PlayerWorks.Read();
			SystemWork.Read();
			SystemWork2.Read();
			Work0.Read();
			Work1.Read();
			WorkWork.Read();
			InetSys.Read();
			McsPsw.Read();
			CameraWork.Read();
		}

		void Write() const {
			// SwCrnt.Write();
			CrrView.Write();
			RandomSeed.Write();
			RandData.Write();
			WorkMoveLast.Write();
			WorkMoveHead.Write();
			HitWork.Write();
			HitWorkWork.Write();
			KRand.Write();
			PlayerWorks.Write();
			SystemWork.Write();
			SystemWork2.Write();
			Work0.Write();
			Work1.Write();
			WorkWork.Write();
			InetSys.Write();
			McsPsw.Write();
			CameraWork.Write();
		}
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

	bool is_ggpo_mode_ = false;
	u16 tsw_tmp_ = 0;
	int game_set_frame_ = 0;
	int rollback_frames_ = 0;
	bool keymsg_updated_ = false;
	bool is_rollbacking_ = false;
	GGPOSession* ggpo_ = nullptr;
	std::array<GGPOPlayerHandle, 4> ggpo_handle_{};
};
