#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "types.h"

#include "gdxsv.pb.h"
#include "gdxsv_backend_tcp.h"
#include "gdxsv_backend_udp.h"
#include "gdxsv_backend_rollback.h"
#include "gdxsv_backend_replay.h"

class Gdxsv {
public:
    enum class NetMode {
        Offline,
        Lbs,
        McsUdp,
        McsRollback,
        Replay,
    };

    Gdxsv() : lbs_net(symbols),
              udp_net(symbols, maxlag),
              rbk_net(symbols, maxlag),
              replay_net(symbols, maxlag) {};

    bool Enabled() const;

    bool InGame() const;

    void Reset();

    void Update();

    void HandleRPC();

    bool StartReplayFile(const char *path);

    bool StartRollbackReplayTest(const char *path);

private:
    static std::string GenerateLoginKey();

    std::vector<u8> GeneratePlatformInfoPacket();

    std::string GeneratePlatformInfoString();

    void WritePatch();

    void WritePatchDisk2();

    void ApplyOnlinePatch(bool first_time);

    void RestoreOnlinePatch();

    NetMode netmode = NetMode::Offline;
    std::atomic<bool> enabled;
    std::atomic<int> disk;
    std::atomic<int> maxlag;

    std::string server;
    std::string loginkey;
    std::map<std::string, u32> symbols;

    proto::GamePatchList patch_list;
    std::vector<proto::ExtPlayerInfo> ext_player_info;

    GdxsvBackendTcp lbs_net;
    GdxsvBackendUdp udp_net;
    GdxsvBackendRollback rbk_net;
    GdxsvBackendReplay replay_net;
};

extern Gdxsv gdxsv;
