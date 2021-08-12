#include "PrecompiledHeader.h"
#include "gdxsv_backend_rollback.h"

#include <array>
#include <vector>
#include <queue>
#include <string>
#include <map>

#include "ggpo/src/include/ggponet.h"

#include "gdx_rpc.h"
#include "gdxsv.pb.h"
#include "gdxsv_ggpo_cb.h"
#include "lbs_message.h"
#include "libs.h"
#include "mcs_message.h"

GdxsvBackendRollback* ptr_GdxsvBackendRollback;

GdxsvBackendRollback::GdxsvBackendRollback(const std::map<std::string, u32> &symbols, std::atomic<int> &maxlag) 
	: symbols_(symbols), maxlag_(maxlag) {
    ptr_GdxsvBackendRollback = this;
}

bool GdxsvBackendRollback::IsReplayTest() {
    return is_replay_test_;
}

void GdxsvBackendRollback::Reset() {
    RestorePatch();
    state_ = State::None;
    is_replay_test_ = false;
    lbs_tx_reader_.Clear();
    mcs_tx_reader_.Clear();
    log_file_.Clear();
    recv_buf_.clear();
    recv_delay_ = 0;
    me_ = 0;
    msg_list_.clear();
    for (int i = 0; i < 4; ++i) {
        key_msg_index_[i].clear();
    }
    std::fill(start_index_.begin(), start_index_.end(), 0);
    std::fill(key_counter_.begin(), key_counter_.end(), 0);
    if (ggpo_ != nullptr) {
        ggpo_close_session(ggpo_);
        ggpo_ = nullptr;
    }
}

bool GdxsvBackendRollback::StartReplayTest(const char* path) {
#ifdef NOWIDE_CONFIG_H_INCLUDED
	FILE* fp = nowide::fopen(path, "rb");
#else
	FILE* fp = fopen(path, "rb");
#endif
	if (fp == nullptr) {
		NOTICE_LOG(COMMON, "fopen failed");
		return false;
	}

	bool ok = log_file_.ParseFromFileDescriptor(fileno(fp));
	if (!ok) {
		NOTICE_LOG(COMMON, "ParseFromFileDescriptor failed");
		return false;
	}

	NOTICE_LOG(COMMON, "game_disk = %s", log_file_.game_disk().c_str());
	fclose(fp);

	McsMessageReader r;
	McsMessage msg;

	if (log_file_.log_file_version() < 20210802) {
		for (int i = 0; i < log_file_.battle_data_size(); ++i) {
			auto data = log_file_.mutable_battle_data(i);
			const auto& fields = proto::BattleLogFile::GetReflection()->GetUnknownFields(*data);
			if (!fields.empty()) {
				for (int j = 0; j < fields.field_count(); ++j) {
					const auto& field = fields.field(j);
					if (j == 0 && field.type() == google::protobuf::UnknownField::TYPE_LENGTH_DELIMITED) {
						const auto& body = field.length_delimited();
						data->set_body(body.data(), body.size());
					}
					if (j == 1 && field.type() == google::protobuf::UnknownField::TYPE_VARINT) {
						data->set_seq(field.varint());
					}
				}
			}
		}

		std::map<std::string, int> player_position;
		for (int i = 0; i < log_file_.battle_data_size(); ++i) {
			const auto& data = log_file_.battle_data(i);
			if (player_position.find(data.user_id()) == player_position.end()) {
				r.Write(data.body().data(), data.body().size());
				while (r.Read(msg)) {
					if (msg.Type() == McsMessage::MsgType::PingMsg) {
						player_position[data.user_id()] = msg.Sender();
						break;
					}
				}
			}
			if (log_file_.users_size() == player_position.size()) {
				break;
			}
		}

		for (int i = 0; i < log_file_.users_size(); ++i) {
			int pos = player_position[log_file_.users(i).user_id()];
			log_file_.mutable_users(i)->set_pos(pos + 1);
			log_file_.mutable_users(i)->set_team(1 + pos / 2);
			// NOTE: Surprisingly, player's grade seems to affect the game.
			log_file_.mutable_users(i)->set_grade(std::min(14, log_file_.users(i).win_count() / 100));
			log_file_.mutable_users(i)->set_user_name_sjis(log_file_.users(i).user_id());
		}

		std::sort(log_file_.mutable_users()->begin(), log_file_.mutable_users()->end(),
			[](const proto::BattleLogUser& a, const proto::BattleLogUser& b) { return a.pos() < b.pos(); });
	}

	msg_list_.clear();
	r.Clear();
	for (int i = 0; i < log_file_.battle_data_size(); ++i) {
		const auto& data = log_file_.battle_data(i);
		r.Write(data.body().data(), data.body().size());
		while (r.Read(msg)) {
			// NOTICE_LOG(COMMON, "MSG:%s", msg.ToHex().c_str());
			if (msg.Type() == McsMessage::KeyMsg2) {
				msg_list_.emplace_back(msg.FirstKeyMsg());
				msg_list_.emplace_back(msg.SecondKeyMsg());
			}
			else {
				msg_list_.emplace_back(msg);
			}
		}
	}

	NOTICE_LOG(COMMON, "users = %d", log_file_.users_size());
	NOTICE_LOG(COMMON, "patch_size = %d", log_file_.patches_size());
	NOTICE_LOG(COMMON, "msg_list.size = %d", msg_list_.size());

	std::fill(start_index_.begin(), start_index_.end(), 0);
    std::fill(key_counter_.begin(), key_counter_.end(), 0);
	state_ = State::Start;
	maxlag_ = 1;
	is_replay_test_ = true;

	NOTICE_LOG(COMMON, "Replay Test Start");
	return true;
}

void GdxsvBackendRollback::Open() {
	gdxsv_WriteMem32(0x0057e734, symbols_.at("gdx_game_body_main"));

	GGPOSessionCallbacks cb = { 0 };
	cb.begin_game = ::ggpo_cb_begin_game;
	cb.advance_frame = ::ggpo_cb_advance_frame;
	cb.load_game_state = ::ggpo_cb_load_game_state;
	cb.save_game_state = ::ggpo_cb_save_game_state;
	cb.free_buffer = ::ggpo_cb_free_buffer;
	cb.on_event = ::ggpo_cb_on_event;
	cb.log_game_state = ::ggpo_cb_log_game_state;

	GGPOErrorCode result;
	if (IsReplayTest()) {
		int test_interval = 1;
		ggpo_start_gdxsv_synctest(&ggpo_, &cb, "gdxsv-ps2", 4, sizeof(GameInput), test_interval);

		for (int i = 0; i < log_file_.users_size(); i++) {
			GGPOPlayer player;
			player.player_num = i + 1;
			player.size = sizeof(GGPOPlayer);
			if (i == 0) {
				player.type = GGPO_PLAYERTYPE_LOCAL;
			}
			else {
				player.type = GGPO_PLAYERTYPE_REMOTE;
				strcpy(player.u.remote.ip_address, "192.168.0.34");
				player.u.remote.port = 30000 + i;
			}

			result = ggpo_add_player(ggpo_, &player, &ggpo_handle_[i]);
			if (player.type == GGPO_PLAYERTYPE_LOCAL) {
				me_ = i;
				// ggpo_set_frame_delay(ggpo, handle, 0);
			}
		}

		ggpo_idle(ggpo_, 0); // must call it at before first add_local_input

		recv_buf_.assign({ 0x0e, 0x61, 0x00, 0x22, 0x10, 0x31, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd });
		state_ = State::McsSessionExchange;
		ApplyPatch(true);
	}

	ggpo_set_disconnect_timeout(ggpo_, 3000);
	ggpo_set_disconnect_notify_start(ggpo_, 1000);
}

void GdxsvBackendRollback::Close() {
	RestorePatch();
	state_ = State::End;
}

u32 GdxsvBackendRollback::HandleRPC(const gdx_rpc_t& rpc) {
	if (rpc.request == GDX_RPC_SOCK_WRITE) {
		return OnSockWrite(rpc.param1, rpc.param2);
	}
	else if (rpc.request == GDX_RPC_SOCK_READ) {
		return OnSockRead(rpc.param1, rpc.param2);
	}
	else if (rpc.request == GDX_RPC_SOCK_POLL) {
		return OnSockPoll();
	}
	else if (rpc.request == GDX_RPC_GAME_BODY_BEGIN) {
		NOTICE_LOG(COMMON, "OnGameBodyBegin");
		is_ggpo_mode_ = true;
		tsw_tmp_ = gdxsv_ReadMem16(0x00870CC0);
		return 0;
	}
	else if (rpc.request == GDX_RPC_GAME_BODY_ROLLBACK) {
		NOTICE_LOG(COMMON, "GDX_RPC_GAME_BODY_ROLLBACK %d", rpc.param1);
		if (rpc.param1 == 0) {
			// Rollback start
			ggpo_begin_rollback(ggpo_, &rollback_frames_);
			is_rollbacking_ = 0 < rollback_frames_;
			return is_rollbacking_;
		}
		else if (rpc.param1 == 1) {
			// Before rollback a frame (return 1 if rollback required)
			return 0 < rollback_frames_;
		}
		else if (rpc.param1 == 2) {
			// After rollback a frame (skip if no rollback)
			u32 swSetRet = gdxsv_ReadMem32(0x005802f8);
			if (swSetRet == 0) {
				WARN_LOG(COMMON, "swSetRet == 0");
			}

			ggpo_advance_frame(ggpo_);
			ggpo_step_rollback(ggpo_);
			rollback_frames_--;
			int game_set = rpc.param2;
			if (game_set) {
				is_rollbacking_ = false;
				is_ggpo_mode_ = false;
				// TODO: check it is confirmed frame
				return 1;
			}
		}
		else if (rpc.param1 == 3) {
			// Rollback end
			if (is_rollbacking_) {
				is_rollbacking_ = false;
				ggpo_end_rollback(ggpo_);
			}
		}
	}
	else if (rpc.request == GDX_RPC_GAME_BODY_END) {
		NOTICE_LOG(COMMON, "OnGameBodyEnd");
		u32 swSetRet = gdxsv_ReadMem32(0x005802f8);
		if (swSetRet == 0) {
			WARN_LOG(COMMON, "swSetRet == 0");
		}

		ggpo_advance_frame(ggpo_);
		ggpo_idle(ggpo_, 0);

		int game_set = rpc.param1;
		if (game_set) {
			is_ggpo_mode_ = false;
			// TODO: check it is confirmed frame
			return 1;
		}
		return 0;
	}
	return 0;
}

bool GdxsvBackendRollback::ggpo_cb_begin_game(const char*) {
	NOTICE_LOG(COMMON, "ggpo_cb_begin_game");
	return true;
}

bool GdxsvBackendRollback::ggpo_cb_advance_frame(int) {
	NOTICE_LOG(COMMON, "ggpo_cb_advance_frame");
	return true;
}

bool GdxsvBackendRollback::ggpo_cb_load_game_state(unsigned char* buffer, int len) {
	NOTICE_LOG(COMMON, "ggpo_cb_load_game_state");
	LoadGameState(*reinterpret_cast<GameState*>(buffer));
	return true;
}

bool GdxsvBackendRollback::ggpo_cb_save_game_state(unsigned char** buffer, int* len, int* checksum, int frame) {
	NOTICE_LOG(COMMON, "ggpo_cb_save_game_state");
	NOTICE_LOG(COMMON, "sizeof(GameState) = %d", sizeof(GameState));
	*len = sizeof(GameState);
	* buffer = reinterpret_cast<unsigned char*>(std::malloc(*len));
	if (!*buffer) {
		return false;
	}

	SaveCurrentGameState(*reinterpret_cast<GameState*>(*buffer));
	return true;
}

void GdxsvBackendRollback::ggpo_cb_free_buffer(void* buffer) {
	std::free(buffer);
	NOTICE_LOG(COMMON, "ggpo_cb_free_buffer");
	return;
}

bool GdxsvBackendRollback::ggpo_cb_on_event(GGPOEvent* info) {
	NOTICE_LOG(COMMON, "ggpo_cb_on_event %d", info->code);
	return true;
}

bool GdxsvBackendRollback::ggpo_cb_log_game_state(char* filename, unsigned char* buffer, int len) {
	NOTICE_LOG(COMMON, "ggpo_cb_log_game_state %s: %s", filename, std::string((const char*)buffer, len).c_str());
	return true;
}

// private

void GdxsvBackendRollback::SaveCurrentGameState(GdxsvBackendRollback::GameState& state) {
	state.Read();
}

void GdxsvBackendRollback::LoadGameState(const GameState& state) {
	state.Write();
}

u32 GdxsvBackendRollback::OnSockWrite(u32 addr, u32 size) {
	if (size == 0) return 0;

	u8 buf[InetBufSize];
	for (int i = 0; i < size; ++i) {
		buf[i] = gdxsv_ReadMem8(addr + i);
	}

	if (state_ <= State::LbsStartBattleFlow) {
		lbs_tx_reader_.Write((const char*)buf, size);
	}
	else {
		mcs_tx_reader_.Write((const char*)buf, size);
	}

	if (state_ <= State::LbsStartBattleFlow) {
		ProcessLbsMessage();
	}
	else {
		ProcessMcsMessage();
	}

	ApplyPatch(false);

	return size;
}

u32 GdxsvBackendRollback::OnSockRead(u32 addr, u32 size) {
	if (state_ <= State::LbsStartBattleFlow) {
		ProcessLbsMessage();
	}

	if (recv_buf_.empty()) {
		return 0;
	}

	int n = std::min<int>(recv_buf_.size(), size);
	for (int i = 0; i < n; ++i) {
		gdxsv_WriteMem8(addr + i, recv_buf_.front());
		recv_buf_.pop_front();
	}
	return n;
}

u32 GdxsvBackendRollback::OnSockPoll() {
	if (state_ <= State::LbsStartBattleFlow) {
		ProcessLbsMessage();
	}
	if (0 < recv_delay_) {
		recv_delay_--;
		return 0;
	}
	return recv_buf_.size();
}

void GdxsvBackendRollback::PrepareKeyMsgIndex() {
	for (int p = 0; p < log_file_.users_size(); ++p) {
		key_msg_index_[p].clear();

		for (int i = start_index_[p]; i < msg_list_.size(); ++i) {
			const auto& msg = msg_list_[i];
			if (msg.Sender() == p) {
				if (!key_msg_index_[p].empty()) {
					if (msg.Type() == McsMessage::MsgType::StartMsg) {
						start_index_[p] = i + 1;
						break;
					}
				}

				if (msg.Type() == McsMessage::MsgType::KeyMsg1) {
					if (!key_msg_index_[p].empty()) {
						verify(msg_list_[key_msg_index_[p].back()].FirstSeq() + 1 == msg.FirstSeq());
					}
					key_msg_index_[p].emplace_back(i);
				}
				verify(msg.Type() != McsMessage::KeyMsg2);
			}
		}
	}
}

void GdxsvBackendRollback::ProcessLbsMessage() {
	if (state_ == State::Start) {
		LbsMessage::SvNotice(LbsMessage::lbsReadyBattle).Serialize(recv_buf_);
		recv_delay_ = 1;
		state_ = State::LbsStartBattleFlow;
	}

	LbsMessage msg;
	if (lbs_tx_reader_.Read(msg)) {
		// NOTICE_LOG(COMMON, "RECV cmd=%04x seq=%d", msg.command, msg.seq);

		if (state_ == State::Start) {
			state_ = State::LbsStartBattleFlow;
		}

		if (msg.command == LbsMessage::lbsLobbyMatchingEntry) {
			LbsMessage::SvAnswer(msg).Serialize(recv_buf_);
			LbsMessage::SvNotice(LbsMessage::lbsReadyBattle).Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskMatchingJoin) {
			int n = log_file_.users_size();
			LbsMessage::SvAnswer(msg).Write8(n)->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskPlayerSide) {
			// camera player id
			me_ = 0;
			LbsMessage::SvAnswer(msg).Write8(me_ + 1)->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskPlayerInfo) {
			int pos = msg.Read8();
			const auto& user = log_file_.users(pos - 1);
			NOTICE_LOG(COMMON, "pos=%d game_param.size=%d", pos, user.game_param().size());
			LbsMessage::SvAnswer(msg).
				Write8(pos)->
				WriteString(user.user_id())->
				WriteBytes(user.user_name_sjis().data(), user.user_name_sjis().size())->
				WriteBytes(user.game_param().data(), user.game_param().size())->
				Write16(user.grade())->
				Write16(user.win_count())->
				Write16(user.lose_count())->
				Write16(0)->
				Write16(user.battle_count() - user.win_count() - user.lose_count())->
				Write16(0)->
				Write16(user.team())->
				Write16(0)->
				Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskRuleData) {
			LbsMessage::SvAnswer(msg).
				WriteBytes(log_file_.rule_bin().data(), log_file_.rule_bin().size())->
				Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskBattleCode) {
			LbsMessage::SvAnswer(msg).
				WriteBytes(log_file_.battle_code().data(), log_file_.battle_code().size())->
				Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskMcsVersion) {
			LbsMessage::SvAnswer(msg).
				Write8(10)->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskMcsAddress) {
			LbsMessage::SvAnswer(msg).
				Write16(4)->Write8(127)->Write8(0)->Write8(0)->Write8(1)->
				Write16(2)->Write16(3333)->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsLogout) {
			state_ = State::McsWaitJoin;
		}

		recv_delay_ = 1;
	}
}

void GdxsvBackendRollback::ProcessMcsMessage() {
	McsMessage msg;
	if (mcs_tx_reader_.Read(msg)) {
		NOTICE_LOG(COMMON, "Read %s %s", McsMessage::MsgTypeName(msg.Type()), msg.ToHex().c_str());
		switch (msg.Type()) {
		case McsMessage::MsgType::ConnectionIdMsg:
			state_ = State::McsInBattle;
			break;
		case McsMessage::MsgType::IntroMsg:
			for (int i = 0; i < log_file_.users_size(); ++i) {
				if (i != me_) {
					auto intro_msg = McsMessage::Create(McsMessage::MsgType::IntroMsg, i);
					std::copy(intro_msg.body.begin(), intro_msg.body.end(), std::back_inserter(recv_buf_));
				}
			}
			break;
		case McsMessage::MsgType::IntroMsgReturn:
			for (int i = 0; i < log_file_.users_size(); ++i) {
				if (i != me_) {
					auto intro_msg = McsMessage::Create(McsMessage::MsgType::IntroMsgReturn, i);
					std::copy(intro_msg.body.begin(), intro_msg.body.end(), std::back_inserter(recv_buf_));
				}
			}
			break;
		case McsMessage::MsgType::PingMsg:
			for (int i = 0; i < log_file_.users_size(); ++i) {
				if (i != me_) {
					auto pong_msg = McsMessage::Create(McsMessage::MsgType::PongMsg, i);
					pong_msg.SetPongTo(me_);
					pong_msg.PongCount(msg.PingCount());
					std::copy(pong_msg.body.begin(), pong_msg.body.end(), std::back_inserter(recv_buf_));
				}
			}
			break;
		case McsMessage::MsgType::PongMsg:
			break;
		case McsMessage::MsgType::StartMsg:
			for (int i = 0; i < log_file_.users_size(); ++i) {
				if (i != me_) {
					auto start_msg = McsMessage::Create(McsMessage::MsgType::StartMsg, i);
					std::copy(start_msg.body.begin(), start_msg.body.end(), std::back_inserter(recv_buf_));
				}
			}
			PrepareKeyMsgIndex();
			break;
		case McsMessage::MsgType::ForceMsg:
			break;
		case McsMessage::MsgType::KeyMsg1: {
			if (is_ggpo_mode_) {
				if (!is_rollbacking_) {
					/*
					GameInput input{};
					input.keycode = tsw_tmp_;
					for (int i = 0; i < 4; ++i) {
						if (ggpo_add_local_input(ggpo_, ggpo_handle_[i], &input, sizeof(input)) == GGPO_OK) {
						}
						else {
							WARN_LOG(COMMON, "ggpo_add_local_input failed");
							verify(false);
						}
					}
					*/

					// Apply replay pad info
					for (int i = 0; i < log_file_.users_size(); ++i) {
						if (msg.FirstSeq() < key_msg_index_[i].size()) {
							auto key_msg = msg_list_[key_msg_index_[i][msg.FirstSeq()]];
                            GameInput input{};
                            input.keycode = u16(key_msg.body[3]) << 8 | key_msg.body[2];
							if (ggpo_add_local_input(ggpo_, ggpo_handle_[i], &input, sizeof(input)) == GGPO_OK) {
							}
							else {
								WARN_LOG(COMMON, "ggpo_add_local_input failed");
								verify(false);
							}
							key_counter_[i] = msg.FirstSeq() + 1;
						}
					}
				}

				// Get current frame pad data from ggpo.
				GameInput inputs[4] = {};
				if (ggpo_synchronize_input(ggpo_, inputs, sizeof(inputs), 0) == GGPO_OK) {
					for (int i = 0; i < log_file_.users_size(); ++i) {
						auto key_msg = McsMessage::Create(McsMessage::KeyMsg1, i);
						key_msg.body[2] = inputs[i].keycode & 0xff;
						key_msg.body[3] = (inputs[i].keycode >> 8) & 0xff;
						NOTICE_LOG(COMMON, "KeyMsg:%s", key_msg.ToHex().c_str());
						std::copy(key_msg.body.begin(), key_msg.body.end(), std::back_inserter(recv_buf_));
					}
				}
				else {
					WARN_LOG(COMMON, "ggpo_synchronize_input failed");
					verify(false);
				}
			} else {
				// TODO: Send KeyMsg

				// Apply replay pad info
				for (int i = 0; i < log_file_.users_size(); ++i) {
					if (msg.FirstSeq() < key_msg_index_[i].size()) {
						auto key_msg = msg_list_[key_msg_index_[i][msg.FirstSeq()]];
						NOTICE_LOG(COMMON, "KeyMsg:%s", key_msg.ToHex().c_str());
						std::copy(key_msg.body.begin(), key_msg.body.end(), std::back_inserter(recv_buf_));
						key_counter_[i] = msg.FirstSeq() + 1;
					}
				}
			}
			break;
		}
		case McsMessage::MsgType::KeyMsg2:
			verify(false);
			break;
		case McsMessage::MsgType::LoadStartMsg:
			INFO_LOG(COMMON, "LoadStartMsg");
			for (int i = 0; i < log_file_.users_size(); ++i) {
				if (i != me_) {
					auto load_start_msg = McsMessage::Create(McsMessage::MsgType::LoadStartMsg, i);
					std::copy(load_start_msg.body.begin(), load_start_msg.body.end(),
						std::back_inserter(recv_buf_));
				}
			}
			break;
		case McsMessage::MsgType::LoadEndMsg:
			INFO_LOG(COMMON, "LoadEndMsg");
			// TODO:ggpo handle
			for (int i = 0; i < log_file_.users_size(); ++i) {
				if (i != me_) {
					auto load_end_msg = McsMessage::Create(McsMessage::MsgType::LoadEndMsg, i);
					std::copy(load_end_msg.body.begin(), load_end_msg.body.end(),
						std::back_inserter(recv_buf_));
				}
			}
			break;
		default:
			WARN_LOG(COMMON, "unhandled mcs msg: %s", McsMessage::MsgTypeName(msg.Type()));
			WARN_LOG(COMMON, "%s", msg.ToHex().c_str());
			break;
		}
	}
}

void GdxsvBackendRollback::ApplyPatch(bool first_time) {
	if (state_ == State::None || state_ == State::End) {
		return;
	}

	// Skip Key MsgPush
	// TODO: disk1
	if (log_file_.game_disk() == "dc2") {
		gdxsv_WriteMem16(0x8c045f64, 9);
		gdxsv_WriteMem8(0x0c3abb90, 1);
	}
	if (log_file_.game_disk() == "ps2") {
		gdxsv_WriteMem32(0x0037f5a0, 0);
		gdxsv_WriteMem8(0x00580340, 1);
	}

	// Online Patch
	for (int i = 0; i < log_file_.patches_size(); ++i) {
		if (log_file_.patches(i).write_once() && !first_time) {
			continue;
		}

		for (int j = 0; j < log_file_.patches(i).codes_size(); ++j) {
			const auto& code = log_file_.patches(i).codes(j);
			if (code.size() == 8) {
				gdxsv_WriteMem8(code.address(), code.changed());
			}
			if (code.size() == 16) {
				gdxsv_WriteMem16(code.address(), code.changed());
			}
			if (code.size() == 32) {
				gdxsv_WriteMem32(code.address(), code.changed());
			}
		}
	}
}

void GdxsvBackendRollback::RestorePatch() {
	if (log_file_.game_disk() == "dc2") {
		gdxsv_WriteMem16(0x8c045f64, 0x410b);
		gdxsv_WriteMem8(0x0c3abb90, 2);
	}

	if (log_file_.game_disk() == "ps2") {
		gdxsv_WriteMem32(0x0037f5a0, 0x0c0e0be4);
		gdxsv_WriteMem8(0x00580340, 2);
	}

	// Online Patch
	for (int i = 0; i < log_file_.patches_size(); ++i) {
		for (int j = 0; j < log_file_.patches(i).codes_size(); ++j) {
			const auto& code = log_file_.patches(i).codes(j);
			if (code.size() == 8) {
				gdxsv_WriteMem8(code.address(), code.original());
			}
			if (code.size() == 16) {
				gdxsv_WriteMem16(code.address(), code.original());
			}
			if (code.size() == 32) {
				gdxsv_WriteMem32(code.address(), code.original());
			}
		}
	}
}
