#include "PrecompiledHeader.h"

#include <fstream>
#include <sstream>
#include <random>
#include <regex>

#include "DebugTools/DebugInterface.h"
#include "App.h"
#include "gdxsv.h"
#include "libs.h"

bool Gdxsv::InGame() const {
    return enabled && netmode == NetMode::McsUdp;
}

bool Gdxsv::Enabled() const {
    return enabled;
}

void Gdxsv::Reset() {
    lbs_net.Reset();
    udp_net.Reset();
    RestoreOnlinePatch();

	// TODO game_id check
    enabled = true;

	static WSADATA wsa{};
	if (wsa.wVersion == 0) {
		printf("Initialising Winsock");
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			printf("WSAStartup failed. error : %d", WSAGetLastError());
		}
	}

    lbs_net.callback_lbs_packet([this](const LbsMessage &lbs_msg) {
        if (lbs_msg.command == LbsMessage::lbsExtPlayerInfo) {
            proto::ExtPlayerInfo player_info;
            if (player_info.ParseFromArray(lbs_msg.body.data(), lbs_msg.body.size())) {
                ext_player_info.push_back(player_info);
            }
        }
        if (lbs_msg.command == LbsMessage::lbsReadyBattle) {
            // Reset current patches for no-patched game
            RestoreOnlinePatch();
            ext_player_info.clear();
        }
        if (lbs_msg.command == LbsMessage::lbsGamePatch) {
            // Reset current patches and update patch_list
            RestoreOnlinePatch();
            if (patch_list.ParseFromArray(lbs_msg.body.data(), lbs_msg.body.size())) {
                ApplyOnlinePatch(true);
            } else {
                printf("patch_list deserialize error");
            }
        }
    });

    // TODO:
    // std::thread([this]() { GcpPingTest(); }).detach();
	server = "localhost";

    wxString app_root = Path::GetDirectory(g_Conf->Folders.Logs.ToString());
    wxString loginkey_path = Path::Combine(app_root, L"gdxsv_loginkey.txt");

	if (wxFileExists(loginkey_path)) {
		std::ifstream ifs(loginkey_path.ToStdString());
		ifs >> loginkey;
	} else {
        loginkey = GenerateLoginKey();
        std::ofstream ofs(loginkey_path.ToStdString());
        ofs << loginkey << std::endl;
    }
}

void Gdxsv::Update() {
    if (!enabled) return;
    WritePatch();
}

std::string Gdxsv::GeneratePlatformInfoString() {
    std::stringstream ss;
    ss << "pcsx2=" << 0.0 << "\n";
    ss << "cpu=" <<
       #if HOST_CPU == CPU_X86
       "x86"
       #elif HOST_CPU == CPU_ARM
       "ARM"
       #elif HOST_CPU == CPU_MIPS
       "MIPS"
       #elif HOST_CPU == CPU_X64
       "x86/64"
       #elif HOST_CPU == CPU_GENERIC
       "Generic"
       #elif HOST_CPU == CPU_ARM64
       "ARM64"
       #else
       "Unknown"
       #endif
       << "\n";
    ss << "os=" <<
       #ifdef __ANDROID__
       "Android"
       #elif defined(__unix__)
       "Linux"
       #elif defined(__APPLE__)
       #ifdef TARGET_IPHONE
       "iOS"
       #else
       "macOS"
       #endif
       #elif defined(_WIN32)
       "Windows"
       #else
       "Unknown"
       #endif
       << "\n";
    ss << "patch_id=" << symbols[":patch_id"] << "\n";
	/*
    std::string machine_id = os_GetMachineID();
    if (machine_id.length()) {
        auto digest = XXH64(machine_id.c_str(), machine_id.size(), 37);
        ss << "machine_id=" << std::hex << digest << std::dec << "\n";
    }
    ss << "wireless=" << (int) (os_GetConnectionMedium() == "Wireless") << "\n";
    ss << "local_ip=" << lbs_net.LocalIP() << "\n";
    // ss << "bind_port=" << .bind_port() << "\n";
	*/
    return ss.str();
}

std::vector<u8> Gdxsv::GeneratePlatformInfoPacket() {
    std::vector<u8> packet = {0x81, 0xff, 0x99, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff};
    auto s = GeneratePlatformInfoString();
    packet.push_back((s.size() >> 8) & 0xffu);
    packet.push_back(s.size() & 0xffu);
    std::copy(std::begin(s), std::end(s), std::back_inserter(packet));
    std::vector<u8> e_loginkey(loginkey.size());
    static const int magic[] = {0x46, 0xcf, 0x2d, 0x55};
    for (int i = 0; i < e_loginkey.size(); ++i) e_loginkey[i] ^= loginkey[i] ^ magic[i & 3];
    packet.push_back((e_loginkey.size() >> 8) & 0xffu);
    packet.push_back(e_loginkey.size() & 0xffu);
    std::copy(std::begin(e_loginkey), std::end(e_loginkey), std::back_inserter(packet));
    u16 payload_size = (u16) (packet.size() - 12);
    packet[4] = (payload_size >> 8) & 0xffu;
    packet[5] = payload_size & 0xffu;
    return packet;
}


void Gdxsv::HandleRPC() {
	u32 gdx_rpc_addr = symbols["gdx_rpc"];
	if (gdx_rpc_addr == 0) {
		return;
	}

	u32 response = 0;
	gdx_rpc_t gdx_rpc{};
	gdx_rpc.request = gdxsv_ReadMem32(gdx_rpc_addr);
	gdx_rpc.response = gdxsv_ReadMem32(gdx_rpc_addr + 4);
	gdx_rpc.param1 = gdxsv_ReadMem32(gdx_rpc_addr + 8);
	gdx_rpc.param2 = gdxsv_ReadMem32(gdx_rpc_addr + 12);
	gdx_rpc.param3 = gdxsv_ReadMem32(gdx_rpc_addr + 16);
	gdx_rpc.param4 = gdxsv_ReadMem32(gdx_rpc_addr + 20);

	if (gdx_rpc.request == GDX_RPC_SOCK_OPEN) {
		u32 tolobby = gdx_rpc.param1;
		u32 host_ip = gdx_rpc.param2;
		u32 port_no = gdx_rpc.param3;

		std::string host = server;
		u16 port = port_no;

		if (netmode == NetMode::Replay) {
			replay_net.Open();
		}
		else if (tolobby == 1) {
			udp_net.CloseMcsRemoteWithReason("cl_to_lobby");
			if (lbs_net.Connect(host, port)) {
				netmode = NetMode::Lbs;
				auto packet = GeneratePlatformInfoPacket();
				lbs_net.Send(packet);
			}
			else {
				netmode = NetMode::Offline;
			}
		}
		else {
			lbs_net.Close();
			host = std::string(inet_ntoa(*(struct in_addr*) & host_ip));
			if (udp_net.Connect(host, port)) {
				netmode = NetMode::McsUdp;
			}
			else {
				netmode = NetMode::Offline;
			}
		}
	}

	if (gdx_rpc.request == GDX_RPC_SOCK_CLOSE) {
		if (netmode == NetMode::Replay) {
			replay_net.Close();
		}
		else {
			lbs_net.Close();

			if (gdx_rpc.param2 == 0) {
				udp_net.CloseMcsRemoteWithReason("cl_app_close");
			}
			else if (gdx_rpc.param2 == 1) {
				udp_net.CloseMcsRemoteWithReason("cl_ppp_close");
			}
			else if (gdx_rpc.param2 == 2) {
				udp_net.CloseMcsRemoteWithReason("cl_soft_reset");
			}
			else {
				udp_net.CloseMcsRemoteWithReason("cl_tcp_close");
			}

			netmode = NetMode::Offline;
		}
	}

	if (gdx_rpc.request == GDX_RPC_SOCK_READ) {
		if (netmode == NetMode::Lbs) {
			response = lbs_net.OnSockRead(gdx_rpc.param1, gdx_rpc.param2);
		}
		else if (netmode == NetMode::McsUdp) {
			response = udp_net.OnSockRead(gdx_rpc.param1, gdx_rpc.param2);
		}
		else if (netmode == NetMode::Replay) {
			response = replay_net.OnSockRead(gdx_rpc.param1, gdx_rpc.param2);
		}
	}

	if (gdx_rpc.request == GDX_RPC_SOCK_WRITE) {
		if (netmode == NetMode::Lbs) {
			response = lbs_net.OnSockWrite(gdx_rpc.param1, gdx_rpc.param2);
		}
		else if (netmode == NetMode::McsUdp) {
			response = udp_net.OnSockWrite(gdx_rpc.param1, gdx_rpc.param2);
		}
		else if (netmode == NetMode::Replay) {
			response = replay_net.OnSockWrite(gdx_rpc.param1, gdx_rpc.param2);
		}
	}

	if (gdx_rpc.request == GDX_RPC_SOCK_POLL) {
		if (netmode == NetMode::Lbs) {
			response = lbs_net.OnSockPoll();
		}
		else if (netmode == NetMode::McsUdp) {
			response = udp_net.OnSockPoll();
		}
		else if (netmode == NetMode::Replay) {
			response = replay_net.OnSockPoll();
		}
	}

	gdxsv_WriteMem32(gdx_rpc_addr, 0);
	gdxsv_WriteMem32(gdx_rpc_addr + 4, response);
	gdxsv_WriteMem32(gdx_rpc_addr + 8, 0);
	gdxsv_WriteMem32(gdx_rpc_addr + 12, 0);
	gdxsv_WriteMem32(gdx_rpc_addr + 16, 0);
	gdxsv_WriteMem32(gdx_rpc_addr + 20, 0);

	gdxsv_WriteMem32(symbols["is_online"], netmode != NetMode::Offline);
}

std::string Gdxsv::GenerateLoginKey() {
    const int n = 8;
    uint64_t seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::mt19937 gen(seed);
    std::string chars = "0123456789";
    std::uniform_int_distribution<> dist(0, chars.length() - 1);
    std::string key(n, 0);
    std::generate_n(key.begin(), n, [&]() {
        return chars[dist(gen)];
    });
    return key;
}

void Gdxsv::ApplyOnlinePatch(bool first_time) {
    for (int i = 0; i < patch_list.patches_size(); ++i) {
        auto &patch = patch_list.patches(i);
        if (patch.write_once() && !first_time) {
            continue;
        }
        if (first_time) {
            printf("patch apply: %s", patch.name().c_str());
        }
        for (int j = 0; j < patch.codes_size(); ++j) {
            auto &code = patch.codes(j);
            if (code.size() == 8) {
                gdxsv_WriteMem8(code.address(), (u8) (code.changed() & 0xff));
            }
            if (code.size() == 16) {
                gdxsv_WriteMem16(code.address(), (u16) (code.changed() & 0xffff));
            }
            if (code.size() == 32) {
                gdxsv_WriteMem32(code.address(), code.changed());
            }
        }
    }
}

void Gdxsv::RestoreOnlinePatch() {
    for (int i = 0; i < patch_list.patches_size(); ++i) {
        auto &patch = patch_list.patches(i);
        printf("patch restore: %s", patch.name().c_str());
        for (int j = 0; j < patch.codes_size(); ++j) {
            auto &code = patch.codes(j);
            if (code.size() == 8) {
                gdxsv_WriteMem8(code.address(), (u8) (code.original() & 0xff));
            }
            if (code.size() == 16) {
                gdxsv_WriteMem16(code.address(), (u16) (code.original() & 0xffff));
            }
            if (code.size() == 32) {
                gdxsv_WriteMem32(code.address(), code.original());
            }
        }
    }
    patch_list.clear_patches();
}

void Gdxsv::WritePatch() {
    WritePatchDisk2();
    if (symbols["patch_id"] == 0 || gdxsv_ReadMem32(symbols["patch_id"]) != symbols[":patch_id"] || gdxsv_ReadMem32(0x005548e0u) != symbols["gdx_dial_start"]) {
        printf("patch %d %d", gdxsv_ReadMem32(symbols["patch_id"]), symbols[":patch_id"]);
#include "gdxsv_patch.inc"
    }
}

void Gdxsv::WritePatchDisk2() {
    // Max Rebattle Patch
    // WriteMem8_nommu(0x0c0219ec, 5);

    // Fix cost 300 to 295
    // WriteMem16_nommu(0x0c21bfec, 295);
    // WriteMem16_nommu(0x0c21bff4, 295);
    // WriteMem16_nommu(0x0c21c034, 295);

    // Reduce max lag-frame
	// InetClntParam
    gdxsv_WriteMem8(0x00580341, maxlag);

	// replace modem_recognition with network_battle.
    gdxsv_WriteMem32(0x003c4f58, 0x0015f110);

    // skip form validation
    gdxsv_WriteMem32(0x003551c0, 0);

    // Write LoginKey
	if (gdxsv_ReadMem8(0x00a88370) == 0) {
		int n = loginkey.length();
		for (int i = 0; i < n; ++i) {
			gdxsv_WriteMem8(0x00a88370 + i, loginkey[i]);
		}
	}

    // Online patch
    ApplyOnlinePatch(false);
}

bool Gdxsv::StartReplayFile(const char *path) {
    replay_net.Reset();
    wxString app_root = Path::GetDirectory(g_Conf->Folders.Logs.ToString());
    wxString replay_path = Path::Combine(app_root, path);
    if (replay_net.StartFile(replay_path.c_str())) {
        netmode = NetMode::Replay;
        return true;
    }
    return false;
}

Gdxsv gdxsv;
