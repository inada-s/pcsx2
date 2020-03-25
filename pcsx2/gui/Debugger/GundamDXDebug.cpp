#include "PrecompiledHeader.h"
#include "GundamDXDebug.h"

#include <map>
#include <string>
#include <functional>

#include "App.h"
#include "DebuggerLists.h"
#include "DebugTools/DebugInterface.h"
#include "DisassemblyDialog.h"
#include "Dump.h"

namespace
{
struct GdxBP;
std::vector<GdxBP> gdx_breakpoints;
std::map<u32, GdxBP> gdx_breakpoints_map;

std::map<std::string, std::function<void(bool, DebugInterface *)>> gdx_remaps_name;
std::map<u32, std::function<void(bool, DebugInterface *)>> gdx_remaps_addr;
const u32 OP_NOP = 0;
const u32 OP_JR_RA = 0x03e00008;

// winsock data
WSADATA wsa;
SOCKET sock = 0;
fd_set fds, readfds;
std::string last_resolved_hostname;

std::mutex io_mtx;
int io_recv_size = 0;
bool io_thread_spawn = false;


// custom break point

struct GdxBP
{
    std::string label;
    u32 addr;
    bool erase;
    bool stop;
    std::function<void()> hook;
};

GdxBP labelBP(const std::string &label, std::function<void()> hook, bool erase = false, bool stop = false)
{
    GdxBP bp;
    bp.label = label;
    bp.erase = erase;
    bp.stop = stop;
    bp.hook = hook;
    return bp;
}

GdxBP addrBP(u32 addr, std::function<void()> hook, bool erase = false, bool stop = false)
{
    GdxBP bp;
    bp.addr = addr;
    bp.erase = erase;
    bp.stop = stop;
    bp.hook = hook;
    return bp;
}


enum {
    REG_r0,
    REG_at,
    REG_v0,
    REG_v1,
    REG_a0,
    REG_a1,
    REG_a2,
    REG_a3,
    REG_t0,
    REG_t1,
    REG_t2,
    REG_t3,
    REG_t4,
    REG_t5,
    REG_t6,
    REG_t7,
    REG_s0,
    REG_s1,
    REG_s2,
    REG_s3,
    REG_s4,
    REG_s5,
    REG_s6,
    REG_s7,
    REG_t8,
    REG_t9,
    REG_k0,
    REG_k1,
    REG_gp,
    REG_sp,
    REG_fp,
    REG_ra,
};

void dump_addr(const char *name, u32 addr)
{
    printf("%s: %08x %s\n", name, addr, symbolMap.GetDescription(addr).c_str());
}

void dump_state()
{
    puts("=== dump_state ===");

    dump_addr("pc", r5900Debug.getPC());
    dump_addr("a0", r5900Debug.getRegister(0, REG_a0)._u32[0]);
    dump_addr("a1", r5900Debug.getRegister(0, REG_a1)._u32[0]);
    dump_addr("a2", r5900Debug.getRegister(0, REG_a2)._u32[0]);
    dump_addr("a3", r5900Debug.getRegister(0, REG_a3)._u32[0]);
    dump_addr("ra", r5900Debug.getRegister(0, REG_ra)._u32[0]);
    puts("");
    puts(">> trace");
    int d = 0;
    for (auto t : MipsStackWalk::Walk((DebugInterface *)&r5900Debug, r5900Debug.getPC(), r5900Debug.getRegister(0, REG_ra)._u32[0], r5900Debug.getRegister(0, REG_sp)._u32[0], 0, 0)) {
        printf("%2d: %08x %s (+%dh)\n", d++, t.pc, symbolMap.GetDescription(t.entry).c_str(), (t.pc - t.entry) / 4);
    }
    puts("=== ---------- ===");
}

void clear_function(u32 addr)
{
    const u32 start = symbolMap.GetFunctionStart(addr);
    if (start == symbolMap.INVALID_ADDRESS) {
        return;
    }
    const u32 size = symbolMap.GetFunctionSize(start);
    assert(start == addr);
    for (int i = 0; i < size; ++i) {
        r5900Debug.write8(start + i, 0);
    }
    r5900Debug.write32(start + size - 8, OP_JR_RA);
}

void clear_function(const char *name)
{
    u32 addr = 0;
    if (symbolMap.GetLabelValue(name, addr)) {
        clear_function(addr);
    }
}

std::string gdx_read_string(u32 address)
{
    if (address == 0) {
        return "";
    }

    char buf[4096] = {0};
    for (int i = 0; i < 4096; ++i) {
        buf[i] = static_cast<char>(r5900Debug.read8(address + i));
        if (buf[i] == 0) {
            break;
        }
    }

    return std::string(buf);
}

} // end of namespace

// ---

void gdx_initialize()
{
    gdx_breakpoints.clear();
    gdx_breakpoints.push_back(labelBP(
        "Ave_SifCallRpc", []() {
            assert(false);
        },
        true));

    gdx_breakpoints.push_back(labelBP(
        "Ave_DnsGetTicket", []() {
        },
        true));

    gdx_breakpoints.push_back(labelBP(
        "Ave_DnsReleaseTicket", []() {
            r5900Debug.setRegister(0, REG_v0, u128::From32(0));
        },
        true));

    gdx_breakpoints.push_back(labelBP(
        "gethostbyname_ps2_0", []() {
            auto hostname = gdx_read_string(r5900Debug.getRegister(0, REG_a0));
            printf("gethostbyname_ps2_0 %s\n", hostname.c_str());
            last_resolved_hostname = hostname;
            r5900Debug.setRegister(0, REG_v0, u128::From32(1));
        },
        true));

    gdx_breakpoints.push_back(labelBP(
        "gethostbyname_ps2_1", []() {
            // unknown
            // auto ticket_number = r5900Debug.read32(r5900Debug.getRegister(0, REG_a0));
            // printf("ticket_number %d\n", ticket_number);
        },
        true));

    gdx_breakpoints.push_back(labelBP(
        "Ave_TcpOpen", []() {
            std::lock_guard<std::mutex> lock(io_mtx);

            printf("TcpOpen:%s\n", last_resolved_hostname.c_str());
            if (last_resolved_hostname.empty()) {
                printf("no host available");
                return;
            }

            puts("connecting to dummy server");
            // local dummy server
            auto hostEntry = gethostbyname("localhost");
            assert(hostEntry);

            if (sock != 0) {
                closesocket(sock);
                sock = 0;
            }
            sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock == INVALID_SOCKET) {
                printf("socket error : %d\n", WSAGetLastError());
                return;
            }
            SOCKADDR_IN addr;
            addr.sin_family = AF_INET;
            addr.sin_addr = *((LPIN_ADDR)hostEntry->h_addr_list[0]);
            addr.sin_port = htons(3333);
            auto err = connect(sock, (const sockaddr *)&addr, sizeof(addr));
            FD_SET(sock, &readfds);

            r5900Debug.setRegister(0, REG_v0, u128::From32(1)); // return socket number
            puts("connecting to dummy server done");
        },
        true));

    gdx_breakpoints.push_back(labelBP(
        "Ave_TcpClose", []() {
            std::lock_guard<std::mutex> lock(io_mtx);
            if (sock != 0) {
                closesocket(sock);
                sock = 0;
            }
        },
        true));

    gdx_breakpoints.push_back(labelBP(
        "Ave_TcpSend", []() {
            char buf[1024] = {0};
            const u32 p = r5900Debug.getRegister(0, REG_a1)._u32[0];
            const u32 len = r5900Debug.getRegister(0, REG_a2)._u32[0];
            for (int i = 0; i < len; ++i) {
                buf[i] = r5900Debug.read8(p + i);
            }
            printf("-- send --\n");
            for (int i = 0; i < len; ++i) {
                printf("%02x", buf[i] & 0xFF);
                if (i + 1 != len)
                    printf(" ");
                else
                    printf("\n");
            }
            printf("--\n");

            io_mtx.lock();
            const int n = send(sock, buf, len, 0);
            io_mtx.unlock();

            if (n != len) {
                printf("unexpected send size\n");
            }
            r5900Debug.setRegister(0, REG_v0, u128::From32(n));
        },
        true));

    gdx_breakpoints.push_back(labelBP(
        "Ave_TcpRecv", []() {
            // int sock_read(sock_t sock, void* buff, const size_t len)
            char buf[1024] = {0};
            // int sock = r5900Debug.getRegister(0, REG_a0)._u32[0];
            const u32 p = r5900Debug.getRegister(0, REG_a1)._u32[0];
            const u32 len = r5900Debug.getRegister(0, REG_a2)._u32[0];
            printf("recv %d bytes\n", len);
            int n = 0;
            {
                std::lock_guard<std::mutex> lock(io_mtx);
                while (n < len) {
                    const int r = recv(sock, buf, len, 0);
                    if (r == SOCKET_ERROR) {
                        r5900Debug.setRegister(0, REG_v0, u128::From32(-1));
                        return;
                    }
                    n += r;
                }
                io_recv_size = 0;
            }
            printf("recv %d bytes done\n", len);

            for (int i = 0; i < n; ++i) {
                r5900Debug.write8(p + i, buf[i]);
            }

            printf("-- recv --\n");
            for (int i = 0; i < len; ++i) {
                printf("%02x", buf[i] & 0xFF);
                if (i + 1 != len)
                    printf(" ");
                else
                    printf("\n");
            }
            printf("--\n");

            r5900Debug.setRegister(0, REG_v0, u128::From32(n));
        },
        true));

    gdx_breakpoints.push_back(labelBP("SetSendCommand", nullptr, false));
    gdx_breakpoints.push_back(labelBP("sock_read", nullptr, false));

    printf("Initialising Winsock");
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed. Error Code : %d", WSAGetLastError());
    }

    {
        std::lock_guard<std::mutex> lock(io_mtx);
        sock = 0;
        FD_ZERO(&readfds);

        if (!io_thread_spawn) {
            io_thread_spawn = true;
            new std::thread([]() {
                printf("io_thread spawn");
                char buf[1024];
                for (;;) {
                    if (sock != 0) {
                        std::lock_guard<std::mutex> lock(io_mtx);

                        memcpy(&fds, &readfds, sizeof(fd_set));
                        timeval timeout;
                        timeout.tv_usec = 1;
                        select(0, &fds, NULL, NULL, &timeout);
                        char buf[1024] = {0};
                        if (FD_ISSET(sock, &fds)) {
                            int n = recv(sock, buf, sizeof(buf), MSG_PEEK);
                            io_recv_size = n;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
            });
        }
    }
}

void gdx_reload_breakpoints()
{
    // TODO force reload
    if (!CBreakPoints::GetBreakpoints().empty()) {
        return;
    }

    auto funcs = symbolMap.GetAllSymbols(ST_FUNCTION);
    if (funcs.empty())
        return;

    printf("gdx_reload_breakpoints\n");

    for (auto &bp : gdx_breakpoints) {
        if (!bp.label.empty()) {
            if (!symbolMap.GetLabelValue(bp.label.c_str(), bp.addr)) {
                printf("failed to get label value %s\n", bp.label.c_str());
                return;
            }
        }
        assert(bp.addr);

        CBreakPoints::AddBreakPoint(bp.addr, false);
        printf("breakpoint added %08x %s\n", bp.addr, bp.label.c_str());
        if (bp.erase) {
            clear_function(bp.addr);
        }
        gdx_breakpoints_map[bp.addr] = bp;
    }

    // -- patch --

    // replace modem_recognition with network_battle.
    r5900Debug.write32(0x003c4f58, 0x0015f110);

    // skip ppp dialing step.
    r5900Debug.write32(0x0035a660, 0x24030002);



    //
    // The game calls these function frequently, so it contribute to too bad performance.
    //

    // rewrite AvepppGetStatus
    /*
	It's too slow
	gdx_breakpoints.push_back(labelBP("AvepppGetStatus", []() {
		r5900Debug.setRegister(0, REG_v0, u128::From32(5)); // I don't know the magic number.
	}, true));
	*/
    // AvepppGetStatus:
    // jr    ra
    // return 0x05
    r5900Debug.write32(0x003584d0, OP_JR_RA);
    r5900Debug.write32(0x003584d4, 0x24020005); // ppp status ok (probably).


    // rewrite Ave_TcpStat
    /*
	It's too slow
	gdx_breakpoints.push_back(labelBP("Ave_TcpStat", []() {
		std::lock_guard<std::mutex> lock(io_mtx);
		if (io_recv_size) {
			r5900Debug.write32(r5900Debug.getRegister(0, REG_a1), 4);
		}
		else {
			r5900Debug.write32(r5900Debug.getRegister(0, REG_a1), 0);
		}
		}, true));
	*/
    // Ave_TcpStat:
    // addiu v0, zero, $000x
    // sw    v0, $0000(a1)
    // jr    ra
    // addiu v0, zero, $0000
    r5900Debug.write32(0x00350990, 0x24020000); // 0: no data comming
    // r5900Debug.write32(0x00350990, 0x24020004); // 4: some data comming (later, change this address dynamically)
    r5900Debug.write32(0x00350994, 0xaca20000);
    r5900Debug.write32(0x00350998, OP_JR_RA);
    r5900Debug.write32(0x0035099c, 0x24020000);
}

void gdx_in_vsync()
{
    if (r5900Debug.isAlive()) {
        if (io_recv_size) {
            r5900Debug.write32(0x00350990, 0x24020004); // 4: some data comming (later, change this address dynamically)
        } else {
            r5900Debug.write32(0x00350990, 0x24020000); // 0: no data comming
        }
    }
}

extern FILE *emuLog;

bool gdx_check_breakpoint()
{
    const u32 pc = r5900Debug.getPC();
    return gdx_breakpoints_map.find(pc) != gdx_breakpoints_map.end();
}

bool gdx_break()
{
    const u32 pc = r5900Debug.getPC();

    dump_state();
    setvbuf(stdout, 0, _IONBF, 0);
    setvbuf(emuLog, 0, _IONBF, 0);

    auto &bp = gdx_breakpoints_map[pc];
    if (bp.hook) {
        bp.hook();
    }

    return bp.stop;
}