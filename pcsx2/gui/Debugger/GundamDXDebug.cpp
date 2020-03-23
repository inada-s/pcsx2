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


//#include "CtrlDisassemblyView.h"
//#include "CtrlRegisterList.h"
//#include "CtrlMemView.h"

//#include "DebugEvents.h"

namespace
{

std::map<std::string, std::function<void(bool, DebugInterface *)>> gdx_remaps_name;
std::map<u32, std::function<void(bool, DebugInterface *)>> gdx_remaps_addr;
std::map<std::string, SymbolEntry> gdx_remaps_symbols;
std::map<u32, SymbolEntry> addr2symbol;

const u32 OP_NOP = 0;
const u32 OP_JR_RA = 0x03e00008;

std::string last_resolved_hostname;

// winsock data
WSADATA wsa;
SOCKET sock = 0;
fd_set fds, readfds;

enum {
    REG_r0, REG_at, REG_v0, REG_v1, REG_a0, REG_a1, REG_a2, REG_a3,
    REG_t0, REG_t1, REG_t2, REG_t3, REG_t4, REG_t5, REG_t6, REG_t7,
    REG_s0, REG_s1, REG_s2, REG_s3, REG_s4, REG_s5, REG_s6, REG_s7,
    REG_t8, REG_t9, REG_k0, REG_k1, REG_gp, REG_sp, REG_fp, REG_ra,
};

std::string gdx_read_string(DebugInterface *cpu, u32 address)
{
    if (address == 0) {
        return "";
    }

    char buf[4096] = {0};
    for (int i = 0; i < 4096; ++i) {
        buf[i] = static_cast<char>(cpu->read8(address + i));
        if (buf[i] == 0) {
            break;
        }
    }

    return std::string(buf);
}

/*
Ave_SifCallRpc
Ave_TcpInitialize
Ave_TcpTerminate
Ave_SetOpt
Ave_GetOpt
Ave_TcpOpen
Ave_TcpClose
Ave_TcpStat
Ave_TcpSend
Ave_TcpRecv
Ave_PppInit
Ave_PppDisp
Ave_PppStart
Ave_PppFinish
Ave_PppStatus
Ave_PppGetUsbDeviceId
Ave_PppSetOption
Ave_PppGetOption
Ave_DnsInit
Ave_DnsDisp
Ave_DnsGetTicket
Ave_DnsReleaseTicket
Ave_DnsLookUp
*/


void Ave_SifCallRpc(bool init, DebugInterface *cpu)
{
    puts(__func__);
    auto func = gdx_remaps_symbols[__func__];

    if (init) {
        // erase the function.
        for (int i = 0; i < func.size; ++i) {
            cpu->write8(func.address + i, 0);
        }
        cpu->write32(func.address, OP_JR_RA);
        return;
    }

	assert(false);
}


void Ave_TcpOpen(bool init, DebugInterface *cpu)
{
    puts(__func__);
    auto func = gdx_remaps_symbols[__func__];

    if (init) {
        // erase the function.
        for (int i = 0; i < func.size; ++i) {
            cpu->write8(func.address + i, 0);
        }
        cpu->write32(func.address, OP_JR_RA);
        return;
    }

    printf("TcpOpen:%s\n", last_resolved_hostname.c_str());
    if (last_resolved_hostname.empty()) {
        printf("no host available");
        return;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        printf("socket error : %d\n", WSAGetLastError());
        return;
    }

	puts("connecting to dummy server");
    // local dummy server
    auto hostEntry = gethostbyname("localhost");
    assert(hostEntry);

    SOCKADDR_IN addr;
    addr.sin_family = AF_INET;
    addr.sin_addr = *((LPIN_ADDR)hostEntry->h_addr_list[0]);
    addr.sin_port = htons(3333);
    auto err = connect(sock, (const sockaddr *)&addr, sizeof(addr));
    assert(err == 0);
    FD_SET(sock, &readfds);

    cpu->setRegister(0, REG_v0, u128::From32(1)); // return socket number
	puts("connecting to dummy server done");
}

void Ave_TcpClose(bool init, DebugInterface *cpu)
{
    puts(__func__);
    auto func = gdx_remaps_symbols[__func__];

    if (init) {
        // erase the function.
        for (int i = 0; i < func.size; ++i) {
            cpu->write8(func.address + i, 0);
        }
        cpu->write32(func.address, OP_JR_RA);
        return;
    }

    if (sock != 0) {
        closesocket(sock);
        sock = 0;
    }
}

void Ave_TcpStat(bool init, DebugInterface *cpu)
{
    puts(__func__);
    auto func = gdx_remaps_symbols[__func__];

    if (init) {
        // erase the function.
        for (int i = 0; i < func.size; ++i) {
            cpu->write8(func.address + i, 0);
        }
        cpu->write32(func.address, OP_JR_RA);
        return;
    }

    // cpu->getRegister(0, REG_a0) // the socket number?

    // return value:
    // -1 : error
    // 0 : no data comming
    // n : how many byts can be recv.
    int n = 0;
    memcpy(&fds, &readfds, sizeof(fd_set));
    timeval timeout;
    timeout.tv_usec = 1;
    select(0, &fds, NULL, NULL, &timeout);
    char buf[1024] = {0};
    if (FD_ISSET(sock, &fds)) {
        n = recv(sock, buf, sizeof(buf), MSG_PEEK); // peek
        printf("recv %d bytes (peek)\n", n);
        cpu->write32(cpu->getRegister(0, REG_a1), 4); // important value
		// cpu->setRegister(0, REG_a1, u128::From32(4)); // important value
    }
	cpu->setRegister(0, REG_v0, u128::From32(n));
}

void Ave_TcpSend(bool init, DebugInterface *cpu)
{
    puts(__func__);
    auto func = gdx_remaps_symbols[__func__];

    if (init) {
        // erase the function.
        for (int i = 0; i < func.size; ++i) {
            cpu->write8(func.address + i, 0);
        }
        cpu->write32(func.address, OP_JR_RA);
        return;
    }

	// int write(sock_t sock, void* buff, const size_t len)
    char buf[1024] = {0};
    // int sock = cpu->getRegister(0, REG_a0)._u32[0];
    const u32 p = cpu->getRegister(0, REG_a1)._u32[0];
    const u32 len = cpu->getRegister(0, REG_a2)._u32[0];
    for (int i = 0; i < len; ++i) {
        buf[i] = cpu->read8(p + i);
    }
    const int n = send(sock, buf, len, 0);
	assert(n == len);
	cpu->setRegister(0, REG_v0, u128::From32(n));
}

void Ave_TcpRecv(bool init, DebugInterface *cpu)
{
    puts(__func__);
    auto func = gdx_remaps_symbols[__func__];

    if (init) {
        // erase the function.
        for (int i = 0; i < func.size; ++i) {
            cpu->write8(func.address + i, 0);
        }
        cpu->write32(func.address, OP_JR_RA);
        return;
    }

	// int sock_read(sock_t sock, void* buff, const size_t len)
    char buf[1024] = {0};
    // int sock = cpu->getRegister(0, REG_a0)._u32[0];
    const u32 p = cpu->getRegister(0, REG_a1)._u32[0];
    const u32 len = cpu->getRegister(0, REG_a2)._u32[0];
    const int n = recv(sock, buf, len, 0);
	assert(n == len);
    for (int i = 0; i < n; ++i) {
        cpu->write8(p + i, buf[i]);
    }
	cpu->setRegister(0, REG_v0, u128::From32(n));
}


void Ave_DnsGetTicket(bool init, DebugInterface *cpu)
{
    puts(__func__);
    auto func = gdx_remaps_symbols[__func__];

    if (init) {
        // erase the function.
        for (int i = 0; i < func.size; ++i) {
            cpu->write8(func.address + i, 0);
        }
        cpu->write32(func.address, OP_JR_RA);
        return;
    }
}

void Ave_DnsReleaseTicket(bool init, DebugInterface *cpu)
{
    puts(__func__);
    auto func = gdx_remaps_symbols[__func__];

    if (init) {
        // erase the function.
        for (int i = 0; i < func.size; ++i) {
            cpu->write8(func.address + i, 0);
        }
        cpu->write32(func.address, OP_JR_RA);
        return;
    }

    // returns -1 means failed
    cpu->setRegister(0, REG_v0, u128::From32(0)); // ?
}


void gethostbyname_ps2_0(bool init, DebugInterface *cpu)
{
    puts(__func__);
    auto func = gdx_remaps_symbols[__func__];

    if (init) {
        // erase the function.
        for (int i = 0; i < func.size; ++i) {
            cpu->write8(func.address + i, 0);
        }
        cpu->write32(func.address, OP_JR_RA);
        return;
    }

    auto hostname = gdx_read_string(cpu, cpu->getRegister(0, REG_a0));
    printf("gethostbyname_ps2_0 %s\n", hostname.c_str());

    last_resolved_hostname = hostname;
    cpu->setRegister(0, REG_v0, u128::From32(1));
}


void gethostbyname_ps2_1(bool init, DebugInterface *cpu)
{
    puts(__func__);
    auto func = gdx_remaps_symbols[__func__];

    if (init) {
        // erase the function.
        for (int i = 0; i < func.size; ++i) {
            cpu->write8(func.address + i, 0);
        }
        cpu->write32(func.address, OP_JR_RA);
        return;
    }

	// unknown
    // auto ticket_number = cpu->read32(cpu->getRegister(0, REG_a0));
    // printf("ticket_number %d\n", ticket_number);
}

void AvepppGetStatus(bool init, DebugInterface* cpu) {
    puts(__func__);
    auto func = gdx_remaps_symbols[__func__];

    if (init) {
        // erase the function.
        for (int i = 0; i < func.size; ++i) {
            cpu->write8(func.address + i, 0);
        }
        cpu->write32(func.address, OP_JR_RA);
        return;
    }

    cpu->setRegister(0, REG_v0, u128::From32(5)); // I don't know the magic number.
}

} // end of namespace

// ---

void gdx_initialize()
{
    gdx_remaps_name["Ave_SifCallRpc"] = Ave_SifCallRpc;

    gdx_remaps_name["Ave_DnsGetTicket"] = Ave_DnsGetTicket;
    gdx_remaps_name["Ave_DnsReleaseTicket"] = Ave_DnsReleaseTicket;

    gdx_remaps_name["gethostbyname_ps2_0"] = gethostbyname_ps2_0;
    gdx_remaps_name["gethostbyname_ps2_1"] = gethostbyname_ps2_1;

    gdx_remaps_name["Ave_TcpOpen"] = Ave_TcpOpen;
    gdx_remaps_name["Ave_TcpClose"] = Ave_TcpClose;
    gdx_remaps_name["Ave_TcpStat"] = Ave_TcpStat;
    gdx_remaps_name["Ave_TcpSend"] = Ave_TcpSend;
    gdx_remaps_name["Ave_TcpRecv"] = Ave_TcpRecv;

    gdx_remaps_name["AvepppGetStatus"] = AvepppGetStatus;


    printf("Initialising Winsock");
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed. Error Code : %d", WSAGetLastError());
    }
    sock = 0;
	FD_ZERO(&readfds);

    gdx_reload_breakpoints();
}

void gdx_reload_breakpoints()
{
    if (!gdx_remaps_addr.empty()) {
        return;
    }

    auto *dlg = wxGetApp().GetDisassemblyPtr();
    if (!dlg) {
        return;
    }

    auto funcs = symbolMap.GetAllSymbols(ST_FUNCTION);
    if (funcs.empty())
        return;

    printf("gdx_reload_breakpoints\n");
    auto cpu = dlg->eeTab->getCpu();

    /*
    for (auto &f : funcs) {
        addr2symbol[f.address] = f;
        if (f.name.substr(0, 4) == "Ave_") {
            puts(f.name.c_str());
        }
    }
	*/

    for (auto &f : funcs) {
        if (gdx_remaps_name.find(f.name) != gdx_remaps_name.end()) {
            CBreakPoints::AddBreakPoint(f.address, false);
            gdx_remaps_addr[f.address] = gdx_remaps_name[f.name];
            gdx_remaps_symbols[f.name] = f;
            printf("breakpoint added %s\n", f.name.c_str());
            gdx_remaps_addr[f.address](true, cpu);
        }
    }

    // replace modem_recognition with network_battle.
    cpu->write32(0x003c4f58, 0x0015f110);

    // skip ppp dialing step
    cpu->write32(0x0035a660, 0x24030002);
}

extern FILE *emuLog;

bool gdx_on_hit_breakpoint(DebugInterface *cpu)
{
    u32 pc = cpu->getPC();

    bool gdx_hit = false;
    if (gdx_remaps_addr.find(pc) != gdx_remaps_addr.end()) {
        gdx_hit = true;

        bool active = !cpu->isCpuPaused();
        if (active) {
            cpu->pauseCpu();
        }

        gdx_remaps_addr[pc](false, cpu);

        if (active) {
            cpu->resumeCpu();
        }
    }

    if (emuLog != NULL) {
        fflush(emuLog);
    }
    return gdx_hit;
}