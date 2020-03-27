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

class GdxTcpClient
{
    SOCKET sock = INVALID_SOCKET;

public:
    void do_connect(const char *host, int port)
    {
        SOCKET new_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (new_sock == INVALID_SOCKET) {
            printf("socket error : %d\n", WSAGetLastError());
            return;
        }

        auto host_entry = gethostbyname(host);
        SOCKADDR_IN addr;
        addr.sin_family = AF_INET;
        addr.sin_addr = *((LPIN_ADDR)host_entry->h_addr_list[0]);
        addr.sin_port = htons(port);
        if (connect(new_sock, (const sockaddr *)&addr, sizeof(addr)) != NO_ERROR) {
            printf("socket error : %d\n", WSAGetLastError());
            return;
        }

        if (sock != INVALID_SOCKET) {
            closesocket(sock);
        }

        sock = new_sock;
    }

    int is_connected()
    {
        return sock != INVALID_SOCKET;
    }

    int do_recv(char *buf, int len)
    {
        return recv(sock, buf, len, 0);
    }

    int do_send(const char *buf, int len)
    {
        return send(sock, buf, len, 0);
    }

    void do_close()
    {
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
    }

    u32 readable_size()
    {
        u_long n = 0;
        ioctlsocket(sock, FIONREAD, &n);
        return u32(n);
    }
};

GdxTcpClient tcp;
struct GdxBP;
std::vector<GdxBP> gdx_breakpoints;
std::map<u32, GdxBP> gdx_breakpoints_map;
std::string last_resolved_hostname;

const u32 OP_NOP = 0;
const u32 OP_JR_RA = 0x03e00008;

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
            tcp.do_connect("localhost", 3333); // dummy
        },
        true));

    gdx_breakpoints.push_back(labelBP(
        "Ave_TcpClose", []() {
            tcp.do_close();
        },
        true));

    gdx_breakpoints.push_back(labelBP(
        "Ave_TcpSend", []() {
            char buf[1024] = {0};
            const u32 p = r5900Debug.getRegister(0, REG_a1)._u32[0];
            const u32 len = r5900Debug.getRegister(0, REG_a2)._u32[0];
            if (len == 0) {
                r5900Debug.setRegister(0, REG_v0, u128::From32(0));
                return;
            }

            for (int i = 0; i < len; ++i) {
                buf[i] = r5900Debug.read8(p + i);
            }

            const int n = tcp.do_send(buf, len);
            if (n == len) {
                r5900Debug.setRegister(0, REG_v0, u128::From32(n));
                printf("-- sent --\n");
            } else {
                r5900Debug.setRegister(0, REG_v0, u128::From32(-1));
            }
        },
        true));

    gdx_breakpoints.push_back(labelBP(
        "Ave_TcpRecv", []() {
            // int sock = r5900Debug.getRegister(0, REG_a0)._u32[0];
            const u32 p = r5900Debug.getRegister(0, REG_a1)._u32[0];
            const u32 len = r5900Debug.getRegister(0, REG_a2)._u32[0];
            printf("try to recv %d bytes\n", len);

            if (tcp.readable_size() < len) {
                puts("[WARN] Not enough length");
                r5900Debug.setRegister(0, REG_v0, u128::From32(-1));
                return;
            }

            char buf[1024];
            int n = tcp.do_recv(buf, len);
            r5900Debug.setRegister(0, REG_v0, u128::From32(n));

            printf("recv %d bytes done\n", n);
            for (int i = 0; i < n; ++i) {
                r5900Debug.write8(p + i, buf[i]);
            }

            printf("-- recv --\n");
            for (int i = 0; i < n; ++i) {
                printf("%02x", buf[i] & 0xFF);
                if (i + 1 != len)
                    printf(" ");
                else
                    printf("\n");
            }
            printf("--\n");
        },
        true));

    // it causes too slow game but the trace is useful for lobby debugging.
    gdx_breakpoints.push_back(labelBP("SetSendCommand", nullptr, false));

    /*
	// client sometimes check msg error code
	gdx_breakpoints.push_back(labelBP("Check_RequestResult",[]() {
		printf("Check_RequestResult %02x\n", r5900Debug.read8(0x00aa64e8));
	}, false));
	*/

    printf("Initialising Winsock");
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed. error : %d", WSAGetLastError());
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
}

//
// patch functions
// The game calls these function frequently, so it contribute to too bad performance.
//

void patch_skip_modem()
{
    // replace modem_recognition with network_battle.
    r5900Debug.write32(0x003c4f58, 0x0015f110);

    // skip ppp dialing step.
    r5900Debug.write32(0x0035a660, 0x24030002);
}

void patch_AvepppGetStatus()
{
    // AvepppGetStatus:
    // return ppp status
    r5900Debug.write32(0x003584d0, OP_JR_RA);
    r5900Debug.write32(0x003584d4, 0x24020005); // v0 = 5 (ppp ok probably)
}

void patch_connect_ps2_check()
{
    // 00357d90: connect_ps2_check
    // return 1 if connected
    r5900Debug.write32(0x00357d90, 0x24020000 | tcp.is_connected());
    r5900Debug.write32(0x00357d94, OP_JR_RA);
    r5900Debug.write32(0x00357d98, OP_NOP);
}

void patch_TcpGetStatus()
{
    // 00381e70: TcpGetStatus
    // args:
    //	a0 : sock
    //	a1 : status result dest
    // returns:
    //  v0 : -1 if no data available
    //  a1[0] : status 0-10 (?) Note: The value is used on original connect_ps2_check.
    //  a1[1] : how many data readable

    u16 retvalue = -1;
    u16 readable_size = 0;
    const int n = tcp.readable_size();
    if (0 < n) {
        retvalue = 0;
        readable_size = n <= 0x7fff ? n : 0x7fff;
    }

    r5900Debug.write32(0x00381e70, 0x24020000);                 // addiu v0, zero, $0000
    r5900Debug.write32(0x00381e74, 0xaca20000);                 // sw    v0, $0000(a1)
    r5900Debug.write32(0x00381e78, 0x24020000 | readable_size); // addiu v0, zero, $0000
    r5900Debug.write32(0x00381e7c, 0xaca20004);                 // sw    v0, $0004(a1)
    r5900Debug.write32(0x00381e80, 0x24020000 | retvalue);      // // sw    v0, $0000(a1)
    r5900Debug.write32(0x00381e84, OP_JR_RA);
    r5900Debug.write32(0x00381e88, OP_NOP);
}

void gdx_in_vsync()
{
    if (r5900Debug.isAlive()) {
        patch_skip_modem();
        patch_AvepppGetStatus();
        patch_connect_ps2_check();
        patch_TcpGetStatus();
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