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

static std::map<std::string, std::function<void(bool, DebugInterface *)>> gdx_remaps_name;
static std::map<u32, std::function<void(bool, DebugInterface *)>> gdx_remaps_addr;
static std::map<std::string, SymbolEntry> gdx_remaps_symbols;
static std::map<u32, SymbolEntry> addr2symbol;

static const u32 OP_NOP = 0;
static const u32 OP_JR_RA = 0x03e00008;

/*
const tMipsRegister MipsRegister[] = {
	{ "r0", 0, 2 }, { "zero", 0, 4}, { "$0", 0, 2 }, { "$zero", 0, 5 },
	{ "at", 1, 2 }, { "r1", 1, 2 }, { "$1", 1, 2 }, { "$at", 1, 3 },
	{ "v0", 2, 2 }, { "r2", 2, 2 }, { "$v0", 2, 3 },
	{ "v1", 3, 2 }, { "r3", 3, 2 }, { "$v1", 3, 3 },
	{ "a0", 4, 2 }, { "r4", 4, 2 }, { "$a0", 4, 3 },
	{ "a1", 5, 2 }, { "r5", 5, 2 }, { "$a1", 5, 3 },
	{ "a2", 6, 2 }, { "r6", 6, 2 }, { "$a2", 6, 3 },
	{ "a3", 7, 2 }, { "r7", 7, 2 }, { "$a3", 7, 3 },
	{ "t0", 8, 2 }, { "r8", 8, 2 }, { "$t0", 8, 3 },
	{ "t1", 9, 2 }, { "r9", 9, 2 }, { "$t1", 9, 3 },
	{ "t2", 10, 2 }, { "r10", 10, 3 }, { "$t2", 10, 3 },
	{ "t3", 11, 2 }, { "r11", 11, 3 }, { "$t3", 11, 3 },
	{ "t4", 12, 2 }, { "r12", 12, 3 }, { "$t4", 12, 3 },
	{ "t5", 13, 2 }, { "r13", 13, 3 }, { "$t5", 13, 3 },
	{ "t6", 14, 2 }, { "r14", 14, 3 }, { "$t6", 14, 3 },
	{ "t7", 15, 2 }, { "r15", 15, 3 }, { "$t7", 15, 3 },
	{ "s0", 16, 2 }, { "r16", 16, 3 }, { "$s0", 16, 3 },
	{ "s1", 17, 2 }, { "r17", 17, 3 }, { "$s1", 17, 3 },
	{ "s2", 18, 2 }, { "r18", 18, 3 }, { "$s2", 18, 3 },
	{ "s3", 19, 2 }, { "r19", 19, 3 }, { "$s3", 19, 3 },
	{ "s4", 20, 2 }, { "r20", 20, 3 }, { "$s4", 20, 3 },
	{ "s5", 21, 2 }, { "r21", 21, 3 }, { "$s5", 21, 3 },
	{ "s6", 22, 2 }, { "r22", 22, 3 }, { "$s6", 22, 3 },
	{ "s7", 23, 2 }, { "r23", 23, 3 }, { "$s7", 23, 3 },
	{ "t8", 24, 2 }, { "r24", 24, 3 }, { "$t8", 24, 3 },
	{ "t9", 25, 2 }, { "r25", 25, 3 }, { "$t9", 25, 3 },
	{ "k0", 26, 2 }, { "r26", 26, 3 }, { "$k0", 26, 3 },
	{ "k1", 27, 2 }, { "r27", 27, 3 }, { "$k1", 27, 3 },
	{ "gp", 28, 2 }, { "r28", 28, 3 }, { "$gp", 28, 3 },
	{ "sp", 29, 2 }, { "r29", 29, 3 }, { "$sp", 29, 3 },
	{ "fp", 30, 2 }, { "r30", 30, 3 }, { "$fp", 30, 3 },
	{ "ra", 31, 2 }, { "r31", 31, 3 }, { "$ra", 31, 3 },
	{ NULL, -1, 0}
};
*/

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

void modem_module_initialize(bool init, DebugInterface *cpu)
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

    cpu->setRegister(0, REG_v0, (u128::From32(0)));
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
}


void Ave_TcpInitialize(bool init, DebugInterface *cpu)
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

void Ave_TcpTerminate(bool init, DebugInterface *cpu)
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

void Ave_SetOpt(bool init, DebugInterface *cpu)
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

void Ave_GetOpt(bool init, DebugInterface *cpu)
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
}

void Ave_PppInit(bool init, DebugInterface *cpu)
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

void Ave_PppDisp(bool init, DebugInterface *cpu)
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

void Ave_PppStart(bool init, DebugInterface *cpu)
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



void Ave_PppFinish(bool init, DebugInterface *cpu)
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

void PppGetStatus(bool init, DebugInterface *cpu)
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

    cpu->setRegister(0, REG_v0, u128::From32(0)); // return status code?.
}

void Ave_PppGetUsbDeviceId(bool init, DebugInterface *cpu)
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

    cpu->setRegister(0, REG_v0, u128::From32(1)); // return device_id.
}

void Ave_PppSetOption(bool init, DebugInterface *cpu)
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

void Ave_PppGetOption(bool init, DebugInterface *cpu)
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

void Ave_DnsInit(bool init, DebugInterface *cpu)
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


void Ave_DnsDisp(bool init, DebugInterface *cpu)
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

void Ave_DnsLookUp(bool init, DebugInterface *cpu)
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

    puts(cpu->getRegister(EECAT_CP0, REG_a0).ToString());
    cpu->setRegister(0, REG_v0, u128::From32(-1)); // ?
}


void dialing_proc(bool init, DebugInterface *cpu)
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

	cpu->setRegister(0, REG_v0, u128::From32(0)); // ?
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
}

} // end of namespace

// ---

void gdx_initialize()
{
    gdx_remaps_name["Ave_SifCallRpc"] = Ave_SifCallRpc;
    gdx_remaps_name["Ave_TcpInitialize"] = Ave_TcpInitialize;
    gdx_remaps_name["Ave_TcpTerminate"] = Ave_TcpTerminate;
    gdx_remaps_name["Ave_SetOpt"] = Ave_SetOpt;
    gdx_remaps_name["Ave_GetOpt"] = Ave_GetOpt;
    gdx_remaps_name["Ave_TcpOpen"] = Ave_TcpOpen;
    gdx_remaps_name["Ave_TcpClose"] = Ave_TcpClose;
    gdx_remaps_name["Ave_TcpStat"] = Ave_TcpStat;
    gdx_remaps_name["Ave_TcpSend"] = Ave_TcpSend;
    gdx_remaps_name["Ave_TcpRecv"] = Ave_TcpRecv;
    /*
    gdx_remaps_name["Ave_PppInit"] = Ave_PppInit;
    gdx_remaps_name["Ave_PppDisp"] = Ave_PppDisp;
    gdx_remaps_name["Ave_PppStart"] = Ave_PppStart;
    gdx_remaps_name["Ave_PppFinish"] = Ave_PppFinish;
    //gdx_remaps_name["Ave_PppStatus"] = Ave_PppStatus;
    gdx_remaps_name["Ave_PppGetUsbDeviceId"] = Ave_PppGetUsbDeviceId;
    gdx_remaps_name["Ave_PppSetOption"] = Ave_PppSetOption;
    gdx_remaps_name["Ave_PppGetOption"] = Ave_PppGetOption;
	*/
    gdx_remaps_name["Ave_DnsInit"] = Ave_DnsInit;
    gdx_remaps_name["Ave_DnsDisp"] = Ave_DnsDisp;
    gdx_remaps_name["Ave_DnsGetTicket"] = Ave_DnsGetTicket;
    gdx_remaps_name["Ave_DnsReleaseTicket"] = Ave_DnsReleaseTicket;
    gdx_remaps_name["Ave_DnsLookUp"] = Ave_DnsLookUp;

    gdx_remaps_name["PppGetStatus"] = PppGetStatus;
    //gdx_remaps_name["dialing_proc"] = dialing_proc;

    gdx_remaps_name["gethostbyname_ps2_0"] = gethostbyname_ps2_0;
    gdx_remaps_name["gethostbyname_ps2_1"] = gethostbyname_ps2_1;


    gdx_reload_breakpoints();
}

void gdx_reload_breakpoints()
{
    if (!gdx_remaps_addr.empty()) {
        return;
    }

    auto funcs = symbolMap.GetAllSymbols(ST_FUNCTION);
    if (funcs.empty())
        return;

    auto *dlg = wxGetApp().GetDisassemblyPtr();
    if (!dlg) {
        return;
    }

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

    // goto network menu
    // cpu->write8(0x008710D0, 6);
    // cpu->write8(0x008710D1, 0);

    // replace modem_recognition -> network_battle
    cpu->write32(0x003c4f58, 0x0015f110);


    // skip dialing step
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