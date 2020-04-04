#include "PrecompiledHeader.h"
#include "GundamDXDebug.h"

#include <random>
#include <map>
#include <string>
#include <functional>
#include <fstream>

#include "App.h"
#include "DebuggerLists.h"
#include "DebugTools/DebugInterface.h"
#include "DisassemblyDialog.h"
#include "Dump.h"

namespace
{


// Apparently, the phone number was used to identify the user.
// We will use the initial value of the connection ID to identify the user.
std::string gdx_get_login_key()
{
    wxString app_root = Path::GetDirectory(g_Conf->Folders.Logs.ToString());
    wxString file_path = Path::Combine(app_root, L"gdxsv_loginkey.txt");

    if (!wxFileExists(file_path)) {
        // generate new login key.
        const int n = 8; // login-key length

        std::random_device rd;
        std::mt19937 gen(rd());
        std::string chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        std::uniform_int_distribution<> dist(0, chars.length() - 1);
        std::string str(n, 0);
        std::generate_n(str.begin(), n, [&]() {
            return chars[dist(gen)];
        });
        std::ofstream ofs(file_path.ToStdString());
        ofs << str << std::endl;
    }

    std::string login_key;
    std::ifstream ifs(file_path.ToStdString());
    ifs >> login_key;
    return login_key;
}

// server address must be set in a file.
std::pair<std::string, int> gdx_get_lobby_addr()
{
    wxString app_root = Path::GetDirectory(g_Conf->Folders.Logs.ToString());
    wxString file_path = Path::Combine(app_root, L"gdxsv_addr.txt");
    if (!wxFileExists(file_path)) {
        printf("file not exists : %s", file_path.c_str());
        return std::make_pair("localhost", 3333);
    }

    std::string host;
    int port = 0;
    std::ifstream ifs(file_path.ToStdString());
    ifs >> host >> port;
    return std::make_pair(host, port);
}

class GdxTcpClient
{
    SOCKET sock = INVALID_SOCKET;

public:
    bool do_connect(const char *host, int port)
    {
        printf("do_connect : %s:%d\n", host, port);

        SOCKET new_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (new_sock == INVALID_SOCKET) {
            printf("socket error : %d\n", WSAGetLastError());
            return false;
        }

        auto host_entry = gethostbyname(host);
        SOCKADDR_IN addr;
        addr.sin_family = AF_INET;
        addr.sin_addr = *((LPIN_ADDR)host_entry->h_addr_list[0]);
        addr.sin_port = htons(port);
        if (connect(new_sock, (const sockaddr *)&addr, sizeof(addr)) != NO_ERROR) {
            printf("socket error : %d\n", WSAGetLastError());
            return false;
        }

        if (sock != INVALID_SOCKET) {
            closesocket(sock);
        }

        sock = new_sock;
        return true;
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
FILE *mylog = nullptr;

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


#define BUFSIZE 4096
u32 gdx_rpc_addr = 0;
u32 gdx_rxq_addr = 0;
u32 gdx_txq_addr = 0;

void find_addr()
{
	// too short tag...
    const int tag_rpc = 0x00637072; // "rpc"
    const int tag_rxq = 0x00717872; // "rxq"
    const int tag_txq = 0x00717874; // "txq"

    for (int i = 0; i < BUFSIZE * 4; ++i) {
        u32 addr = 0x000f0000 + i * 4;
        u32 vale = r5900Debug.read32(addr);
        if (vale == tag_rpc) {
            gdx_rpc_addr = addr;
        }
        if (vale == tag_rxq) {
            gdx_rxq_addr = addr;
        }
        if (vale == tag_txq) {
            gdx_txq_addr = addr;
        }
    }
}

struct gdx_queue
{
    char name[4];
    u32 head;
    u32 tail;
    u32 buf; // u8 buf[BUFSIZE];
};

u32 gdx_queue_size(struct gdx_queue *q)
{
    return (q->tail + BUFSIZE - q->head) % BUFSIZE;
}

void gdx_queue_push(struct gdx_queue *q, u8 data)
{
    r5900Debug.write8(q->buf + q->tail, data);
    q->tail = (q->tail + 1) % BUFSIZE;
}

u8 gdx_queue_pop(struct gdx_queue *q)
{
    u8 ret = r5900Debug.read8(q->buf + q->head);
    q->head = (q->head + 1) % BUFSIZE;
    return ret;
}

void update_gdx_queue()
{
    if (gdx_rxq_addr == 0 || gdx_txq_addr == 0) {
        find_addr();
    }

    if (gdx_txq_addr == 0 || gdx_txq_addr == 0) {
        return;
    }

    if (!tcp.is_connected()) {
        return;
    }

    gdx_queue q;
    char buf[BUFSIZE];
    u32 n;

    n = tcp.readable_size();
    q.head = r5900Debug.read32(gdx_rxq_addr + 4);
    q.tail = r5900Debug.read32(gdx_rxq_addr + 8);
    q.buf = gdx_rxq_addr + 12;
    if (n) {
        n = tcp.do_recv(buf, n);
        for (int i = 0; i < n; ++i) {
            gdx_queue_push(&q, buf[i]);
        }
        r5900Debug.write32(gdx_rxq_addr + 8, q.tail);
    }

    q.head = r5900Debug.read32(gdx_txq_addr + 4);
    q.tail = r5900Debug.read32(gdx_txq_addr + 8);
    q.buf = gdx_txq_addr + 12;
    n = gdx_queue_size(&q);
    if (n) {
        for (int i = 0; i < n; ++i) {
            buf[i] = gdx_queue_pop(&q);
        }
        tcp.do_send(buf, n);
        r5900Debug.write32(gdx_txq_addr + 4, 0);
        r5900Debug.write32(gdx_txq_addr + 8, 0);
    }
}
enum {
    RPC_TCP_OPEN = 1,
    RPC_TCP_CLOSE = 2,
};

struct gdx_rpc_t
{
    char tag[4];
    u32 request;
    u32 response;

    u32 param1;
    u32 param2;
    u32 param3;
    u32 param4;
    u8 name1[128];
    u8 name2[128];
};

void update_gdx_rpc()
{
    if (gdx_rpc_addr == 0) {
        find_addr();
    }

    if (gdx_rpc_addr == 0) {
        return;
    }

    gdx_rpc_t gdx_rpc;
	gdx_rpc.request = r5900Debug.read32(gdx_rpc_addr + 4);
	gdx_rpc.response = r5900Debug.read32(gdx_rpc_addr + 8);
	gdx_rpc.param1 = r5900Debug.read32(gdx_rpc_addr + 12);
	gdx_rpc.param2 = r5900Debug.read32(gdx_rpc_addr + 16);
	gdx_rpc.param3 = r5900Debug.read32(gdx_rpc_addr + 20);
	gdx_rpc.param4 = r5900Debug.read32(gdx_rpc_addr + 24);

	if (gdx_rpc.request == RPC_TCP_OPEN) {
		u32 tolobby = gdx_rpc.param1;
		u32 host_ip = gdx_rpc.param2;
		u32 port_no = gdx_rpc.param3;

		auto host_port = gdx_get_lobby_addr();

		if (tolobby != 1) {
			union {
				u32 _u32;
				u8 _u8[4];
			} ipv4addr;
			ipv4addr._u32 = host_ip;
			auto ip = ipv4addr._u8;
			char buf[1024] = {0};
			sprintf(buf, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
			host_port = std::make_pair(std::string(buf), port_no);
		}

		bool ok = tcp.do_connect(host_port.first.c_str(), host_port.second);
		if (!ok) {
			printf("[WARN] Failed to connect %s:%d\n", host_port.first.c_str(), host_port.second);
		}
	}
	if (gdx_rpc.request == RPC_TCP_CLOSE) {
		tcp.do_close();
	}

	r5900Debug.write32(gdx_rpc_addr + 4, 0);
}

} // end of namespace

// ---

void gdx_initialize()
{
    gdx_get_lobby_addr();
    gdx_get_login_key();
    if (!mylog) {
        mylog = fopen("mylog.txt", "w");
    }

    gdx_breakpoints.clear();

    gdx_breakpoints.push_back(labelBP(
        "Ave_SifCallRpc", []() {
            puts("==============  WARNING ===============");
            puts("=========  Ave_SifCallRpc   ===========");
            puts("==============  WARNING ===============");
        },
        true));

    gdx_breakpoints.push_back(labelBP(
        "McsSifCallRpc", []() {
            puts("==============  WARNING ===============");
            puts("=========  McsSifCallRpc   ===========");
            puts("==============  WARNING ===============");
        },
        true));


    // it causes too slow game but the trace is useful for lobby debugging.
    gdx_breakpoints.push_back(labelBP("SetSendCommand", nullptr, false));
    // gdx_breakpoints.push_back(labelBP("NetHeyaDataSet", nullptr, false, true));
    // gdx_breakpoints.push_back(labelBP("NetRecvHeyaBinDef", nullptr, false, true));

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
//


void patch_connection_id()
{
    const u32 addr = 0x00A8760A;
    if (r5900Debug.read8(addr) == 0) {
        auto login_key = gdx_get_login_key();
        int n = login_key.length();
        for (int i = 0; i < n; ++i) {
            r5900Debug.write8(addr + i, login_key[i]);
        }
        r5900Debug.write8(n, 0);
    }
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
    r5900Debug.write32(0x00381e80, 0x24020000 | retvalue);      // sw    v0, $0000(a1)
    r5900Debug.write32(0x00381e84, OP_JR_RA);
    r5900Debug.write32(0x00381e88, OP_NOP);
}



void gdx_in_vsync()
{
    if (r5900Debug.isAlive()) {
        update_gdx_queue();
        update_gdx_rpc();
        patch_connection_id();
        patch_AvepppGetStatus();
        patch_connect_ps2_check();
        patch_TcpGetStatus();

        static u32 phase = 0;
        auto new_phase = r5900Debug.read32(0x00580138);
        if (phase != new_phase) {
            phase = new_phase;
            printf("game_start_to_mcs_phase: %08x\n", phase);
        }

        // use dummy user?
        // r5900Debug.write32(0x0038b3ec, OP_NOP);
        // r5900Debug.write32(0x0038b3f0, OP_NOP);
    }
}

bool gdx_check_breakpoint()
{
    const u32 pc = r5900Debug.getPC();
    return gdx_breakpoints_map.find(pc) != gdx_breakpoints_map.end();
}

extern FILE *emuLog;

bool gdx_break()
{
    const u32 pc = r5900Debug.getPC();

    dump_state();
    dump_addr("TicketID (DNS)", r5900Debug.read32(0x00580180));
    dump_addr("COM_R_No_2", r5900Debug.read32(0x0057fee0));
    dump_addr("COM_R_No_0", r5900Debug.read32(0x0057fee8)); // jump table 0035a750 modem_connect

    setvbuf(stdout, 0, _IONBF, 0);
    setvbuf(emuLog, 0, _IONBF, 0);

    auto &bp = gdx_breakpoints_map[pc];
    if (bp.hook) {
        bp.hook();
    }

    return bp.stop;
}


void gdx_on_load_irx(std::string name, u32 import_table, u16 index)
{
    /*
	fprintf(mylog, "=========\n");
	fprintf(mylog, "gdx_on_load_irx %s table:%08x index:%04x\n", name.c_str(), import_table, index);

	for (int i = 0; i < 16; ++i) {
		for (int j = 0; j < 16; ++j) {
			fprintf(mylog, "%02x ", r3000Debug.read8(import_table + i*16 + j) & 0xFF);
		}
		for (int j = 0; j < 16; ++j) {
			char c = r3000Debug.read8(import_table + i * 16 + j) & 0xFF;
			fprintf(mylog, "%c", isprint(c) ? c : '.');
		}
		fprintf(mylog, "\n");
	}
*/
}