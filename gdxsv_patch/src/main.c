#include <stdarg.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define GDXDATA __attribute__((section("gdx.data")))
#define GDXFUNC __attribute__((section("gdx.func")))

#define GDX_QUEUE_SIZE 1024
#define OP_NOP 0
#define OP_JR_RA 0x03e00008

#define read8(a) *((u8*)(a))
#define read16(a) *((u16*)(a))
#define read32(a) *((u32*)(a))
#define write8(a, b) *((u8*)(a)) = (b)
#define write16(a, b) *((u16*)(a)) = (b)
#define write32(a, b) *((u32*)(a)) = (b)

enum {
    GDX_RPC_SOCK_OPEN = 1,
    GDX_RPC_SOCK_CLOSE = 2,
    GDX_RPC_SOCK_READ = 3,
    GDX_RPC_SOCK_WRITE = 4,
    GDX_RPC_SOCK_POLL = 5,
};

struct gdx_rpc_t {
    u32 request;
    u32 response;
    u32 param1;
    u32 param2;
    u32 param3;
    u32 param4;
    u8 name1[128];
    u8 name2[128];
};

int gdx_debug_print GDXDATA = 0;
int gdx_initialized GDXDATA = 0;

GDXDATA u32 patch_id = 0;
GDXDATA u32 disk = 0;
GDXDATA u32 is_online = 0;
struct gdx_rpc_t gdx_rpc GDXDATA = {0};

u32 GDXFUNC gdx_rpc_call(u32 request, u32 param1, u32 param2, u32 param3, u32 param4) {
    gdx_rpc.request = request;
    gdx_rpc.response = 0;
    gdx_rpc.param1 = param1;
    gdx_rpc.param2 = param2;
    gdx_rpc.param3 = param3;
    gdx_rpc.param4 = param4;
    ((u32 (*)())0x00103f30)(0, 0);
    return gdx_rpc.response;
}

int GDXFUNC net_FontDisp(const char* text, float x, float y) {
    return ((int (*)(const char *, float, float))0x00378430)(text, x, y);
}

int GDXFUNC gdx_debug(const char *format, ...) {
  if (gdx_debug_print) {
    va_list arg;
    int done;
    va_start(arg, format);
    done = ((int (*)(int *, const char *, va_list arg))0x001192b8)(0x003a73c4, format, arg);
    va_end(arg);
    return done;
  }
}

int GDXFUNC gdx_info(const char *format, ...) {
  va_list arg;
  int done;
  va_start(arg, format);
  done = ((int (*)(int *, const char *, va_list arg))0x001192b8)(0x003a73c4, format, arg);
  va_end(arg);
  return done;
}

u32 GDXFUNC OP_JAL(u32 addr) {
  return 0x0c000000 + addr / 4;
}

u32 GDXFUNC gdx_TcpGetStatus(u32 sock, u32 dst) {
  u32 retvalue = -1;
  u32 readable_size = 0;

  const int n = gdx_rpc_call(GDX_RPC_SOCK_POLL, 0, 0, 0, 0);
  if (0 < n) {
      retvalue = 0;
      readable_size = n <= 0x7fff ? n : 0x7fff;
  }
  write32(dst, 0);
  write32(dst + 4, readable_size);

  gdx_debug("gdx_TcpGetStatus retvalue=%d\n", (int)retvalue);
  return retvalue;
}

u32 GDXFUNC gdx_Ave_TcpSend(u32 sock, u32 ptr, u32 len) {
  int i;
  gdx_debug("gdx_Ave_TcpSend sock:%d ptr:%08x size:%d\n", sock, ptr, len);
  if (len == 0) {
    return 0;
  }

  gdx_debug("send:");
  for (i = 0; i < len; ++i) {
    gdx_debug("%02x ", read8(ptr + i));
  }
  gdx_debug("\n");

  return gdx_rpc_call(GDX_RPC_SOCK_WRITE, ptr, len, 0, 0);
}

u32 GDXFUNC gdx_Ave_TcpRecv(u32 sock, u32 ptr, u32 len) {
  gdx_debug("gdx_Ave_TcpRecv sock:%d ptr:%08x size:%d\n", sock, ptr, len);
  // TODO: should return -1?
  if (len == 0) return 0;
  return gdx_rpc_call(GDX_RPC_SOCK_READ, ptr, len, 0, 0);
}

u32 GDXFUNC gdx_McsReceive(u32 ptr, u32 len) {
  gdx_debug("gdx_McsReceive ptr:%08x size:%d\n", ptr, len);
  if (len == 0) return 0;
  return gdx_rpc_call(GDX_RPC_SOCK_READ, ptr, len, 0, 0);
}

u32 GDXFUNC gdx_McsDispose() {
    return 0;
}

u32 GDXFUNC gdx_Ave_TcpOpen(u32 ip, u32 port) {
  gdx_info("gdx_Ave_TcpOpen\n");
  port = port >> 8 | (port & 0xFF) << 8; // endian fix
  gdx_rpc_call(GDX_RPC_SOCK_OPEN, ip == 0x0077, ip, port, 0);
  return 7; // dummy socket id
}

u32 GDXFUNC gdx_Ave_TcpClose(u32 sock) {
  gdx_info("gdx_Ave_TcpClose\n");
  gdx_rpc_call(GDX_RPC_SOCK_CLOSE, 0, 0, 0, 0);
  return 0;
}

void GDXFUNC gdx_AvepppGetStatus() {
    return 5;
}

void GDXFUNC gdx_Ave_PppGetUsbDeviceId() {
    return 0;
}

void GDXFUNC gdx_ADNS_Finalize() {
    return 0;
}

void GDXFUNC gdx_connect_ps2_check() {
    return is_online;
}

void GDXFUNC gdx_LobbyToMcsInitSocket() {
}

u32 GDXFUNC gdx_gethostbyname_ps2_0(u32 hostname) {
  gdx_info("gdx_gethostbyname_ps2_0\n");
  // hostname : "ca1202.mmcp6"
  return 7; // dummy ticket id
}

u32 GDXFUNC gdx_gethostbyname_ps2_1(u32 ticket_id) {
  gdx_info("gdx_gethostbyname_ps2_1\n");
  return 0x0077; // dummy lobby ip addr
}

u32 GDXFUNC gdx_gethostbyname_ps2_release(u32 ticket_id) {
  gdx_info("gdx_gethostbyname_ps2_release\n");
  return 0; // ok
}

void GDXFUNC write_patch() {
  gdx_debug("write_patch!!!\n");

  // replace modem_recognition with network_battle.
  // write32(0x003c4f58, 0x0015f110);

  // skip ppp dialing step.
  // write32(0x0035a660, 0x24030002);

  // replace network functions.
  write32(0x00381da4, OP_JAL(gdx_Ave_TcpOpen));
  write32(0x00382024, OP_JAL(gdx_Ave_TcpClose));
  write32(0x00381fb4, OP_JAL(gdx_Ave_TcpSend));
  write32(0x00381f7c, OP_JAL(gdx_Ave_TcpRecv));
  write32(0x0037fd2c, OP_JAL(gdx_McsReceive));
  write32(0x00381a88, OP_JAL(gdx_McsDispose));
  write32(0x00382c6c, OP_JAL(gdx_McsDispose));
  write32(0x00357e34, OP_JAL(gdx_TcpGetStatus));
  write32(0x0035a174, OP_JAL(gdx_LobbyToMcsInitSocket));
  write32(0x00359e04, OP_JAL(gdx_gethostbyname_ps2_0));
  write32(0x00359e78, OP_JAL(gdx_gethostbyname_ps2_1));
  write32(0x00359ea4, OP_JAL(gdx_gethostbyname_ps2_release));
  write32(0x00359ec4, OP_JAL(gdx_gethostbyname_ps2_release));
  write32(0x003597b8, OP_JAL(gdx_AvepppGetStatus));
  write32(0x003643c8, OP_JAL(gdx_AvepppGetStatus));
  write32(0x00382678, OP_JAL(gdx_Ave_PppGetUsbDeviceId));
  write32(0x003828e0, OP_JAL(gdx_Ave_PppGetUsbDeviceId));
  write32(0x00359d34, OP_JAL(gdx_ADNS_Finalize));

  write32(0x00359fd8, OP_JAL(gdx_connect_ps2_check));
}

void GDXFUNC gdx_dial_start() {
  gdx_debug("gdx_dial_start\n");

  // COM_R_NO2 = 2
  write32(0x0057fee8, 2);

  if (gdx_initialized) {
    gdx_debug("already initialized\n");
    return;
  }

  write_patch();
  gdx_initialized = 1;
}
