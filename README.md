# PCSX2 for zdxsv

A fork of [PCSX2](https://github.com/PCSX2/pcsx2) for playing Mobile Suit Gundam: Federation vs. Zeon DX (PS2) online
on [zdxsv](https://github.com/inada-s/zdxsv). Player setup: [ZDXSV.md](ZDXSV.md).

- Upstream base: PCSX2 `fd9d310c`. Main branch: `zdxsv-master`. Work branch: `ai/zdxsv`.
- License, BIOS requirement, and everything not listed here: same as upstream PCSX2.

## Changes from upstream PCSX2

### Online play (DEV9 network adapter)
- Network defaults point at zdxsv: adapter on, Sockets API, `Auto` device, DHCP intercepted, internal DNS.
  The game's server hosts (`www01.kddi-mmbb.jp`, `gate1.jp.dnas.playstation.org`, `ca1202.mmcp6`, `ca1203.mmcp6`)
  resolve to the zdxsv server.
- Platform info: on the lobby's first key question, the emulator sends the lobby a custom message (`0x9950`) with
  `key=value` lines (emulator, UDP addresses), so the server can tell emulators from real PS2s. The game never sees it.
- UDP battle bridge (`pcsx2/DEV9/Zdxsv`): zdxsv's `zproxy` built in. Battle traffic goes over zdxsv's UDP protocol to
  the battle server, and straight to peers that answer a ping (P2P). The public address comes from the lobby's STUN.
- Lobby save states (opt-in, debugging, see `ZDXSV_LOBBY_STATE`): save states also keep the network adapter (DEV9
  registers, SMAP buffers), and after a load the emulator takes over the TCP connections the PS2 had opened.

### Input latency
- Low-latency vsync (on by default): the finished frame is presented before the frame limiter sleeps, and input is
  polled right before the next frame runs (about -14 ms from button press to screen).
  Setting: `[EmuCore/GS] ZdxsvLowLatencyVsync = true|false` in `PCSX2.ini`.

### Rollback netcode (GGPO, in progress)
- [GGPO](https://github.com/pond3r/ggpo) in `3rdparty/ggpo` (MIT), imported from inada-s/flycast, with
  `ggpo_get_last_confirmed_frame` added.
- Delta save states (`ZdxsvDeltaState`): a save copies only the EE RAM pages written since the last save (found with
  page write protection), plus the rest of the state. Fast enough to save every frame.
  - Supporting changes in the core: saved scratch fields are skipped in the comparison, counters and the GIF path keep
    their bookkeeping when a delta state loads, and SPU2 DMA IRQ flags are part of the state.
  - The EE and IOP recompilers end blocks only at branches and page boundaries while GGPO runs, so a rerun after a
    rollback times events the same way as the first run.
- GGPO session in a running game (`ZdxsvGgpo`): the Gundam battle runs over GGPO with GGPO input delay 2 and game
  input delay 0. Off unless `ZDXSV_GGPO` is set. Not yet used by the zdxsv release.

### Releases and updates
- The auto-updater checks this fork's GitHub releases (`zdxsv-X.Y.Z` tags) instead of pcsx2.net.
- `zdxsv_release.yml`: pushing a tag `zdxsv-X.Y.Z` builds Windows x64 and publishes `.7z` (updater) and `.zip`
  (first install). A tag with a suffix (`zdxsv-0.0.1-rc1`) becomes a prerelease, which the updater skips.

### Tools and CI
- PINE commands for test rigs: `0x30` set pad input (pad u8, bind u8, value u8), `0x31` queue a GS screenshot
  (path len u16, path), `0x32` read the frame count (u32).
- Unit tests: `tests/ctest/core/ggpo_tests.cpp` (GGPO synctest), `dev9_config_tests.cpp` (network defaults).
- CI: one Windows build (CMake + clang-cl) on pushes to and PRs into `zdxsv-master` and `ai/zdxsv`. The Linux and
  macOS workflows only run by hand (`workflow_dispatch`).

## Options

All options are environment variables, read at startup. None is needed to play.
Test scripts that use them: `zdxsv/` in inada-s/ai-automation (named in brackets).

### Network
- `ZDXSV_UDP=0`: UDP bridge off; the game talks TCP to the battle server.
- `ZDXSV_STUN_PORT=port`: the lobby's STUN port (default 8201).
- `ZDXSV_PLATFORM_INFO=0`: send no platform info; the server sees a real PS2.
- `ZDXSV_LOBBY_STATE=1`: save states made online keep working after a load (`launch.ps1`).
- `ZDXSV_UPDATE_URL=url`: the updater reads its release list from this URL (testing updates).
- `ZDXSV_UDP_DUMP=dir`: write every battle message the game sends (`S`) or gets (`R`) to `dir/bridge-<user>.txt`,
  one line each: `frame ms S|R user seq len hex` (`msgdump.py`, `cmptrace.py`).
- Bridge fault injection (`m4.sh`, `bridge_test/e2e.sh`):
  - `ZDXSV_UDP_TEST_DROP=n`: drop every n-th UDP packet.
  - `ZDXSV_UDP_TEST_P2P_ONLY=1`: battle data only over P2P, not the server relay.
  - `ZDXSV_UDP_TEST_P2P_BLOCK=1`: no P2P; server relay only.
  - `ZDXSV_UDP_TEST_P2P_DELAY=ms`: delay P2P packets.

### Input latency measurement
- `ZDXSV_INPUT_LATENCY="key=value,..."` (any value turns it on): presses a button on pad 1 and logs the time of each
  stage: press, host poll, game pad read, RAM change, present (`latency.ps1`, `m4lat.sh`).
  - `btn=down`, `back=up`: button of even / odd presses (`none` = always `btn`).
  - `addr=0x..`: EE byte the press changes; without it, search mode finds that byte.
  - `count=20`, `start=600` (vsync of the first press), `go=path` (start 60 vsyncs after this file appears),
    `hold=4`, `gap=60` (frames), `held=1` (search for a byte that differs only while held), `seed=1`, `out=path` (CSV).

### Delta state self-test
- `ZDXSV_DELTA_TEST="key=value,..."` (any value turns it on): from vsync `start`, save every frame; every `every`
  frames load the frame `depth` back and rerun. Each rerun frame must hash like its first run. Log lines `ZdxsvDelta`
  (`deltatest.ps1`).
  - `start=3000`, `frames=1800`, `depth=8`, `every=20`.
  - Controls (must report mismatches): `break=ee` (a load does not restore EE RAM), `blocks=linked` (recompiler blocks
    end as upstream).
- `ZDXSV_DELTA_HOT=0`: control; every written page is write-protected each frame (no hot-page copy).

### GGPO
- `ZDXSV_GGPO="key=value,..."`: GGPO session in a running game. Log lines `ZdxsvGgpo`.
  - Synctest (default): from vsync `start=1500`, `frames=3000`; every `check=6` frames GGPO loads back and compares
    state hashes. Input: `seed=1` random, `input=host` (pad 1 from the host), `input=none`; `mask=fcff` (allowed
    buttons, hex). Control: `control=input` (reruns get other inputs; must report mismatches).
  - `net=1`: a battle between `players=4` peers, started when the game opens its battle socket. Peer at position p
    listens on UDP `port=7001` + p on `host=127.0.0.1`; `relay=R`: remote p at R + 8 * me + p (`udprelay.py`).
    `delay=0`: GGPO input delay. `sync=0`: no state hashes (always with `net=1`).
- `ZDXSV_RBK=i/N`: rollback test (`rbk.sh`, `rbkprep.sh`). Start from a post-entry save state as battle position i of
  N. The emulator answers the lobby itself, runs turbo until the battle, and exits when the GGPO session ends.
  - `ZDXSV_RBK_TIME=s` (time limit), `ZDXSV_RBK_COUNT=n` (battles), `ZDXSV_RBK_GAUGE=v` (戦力ゲージ),
    `ZDXSV_RBK_TURBO=1` (the battle runs turbo too).
- `ZDXSV_RAND_INPUT=seed`: seeded random pad input.
- `ZDXSV_ZDS_PS=1`: play-start barrier; every peer starts the battle on the same GGPO frame. Leave it off only as a
  control (peers desync at play start).
- `ZDXSV_K3_LAG=n` (default 8): GGPO frames between the game sending a round-handshake message and the input that
  carries it. `0` is a control (the battle depends on network timing).
- `ZDXSV_SAVE_ALL=1`: control; delta-save every GGPO frame, also frames that can no longer be rolled back.
- `ZDXSV_NET_TAIL=n`: frames run after the battle end message before the session stops (default 300).
- `ZDXSV_NET_DISCONNECT_MS=ms`: GGPO disconnect timeout (default 5000).
- Sync checks:
  - `ZDXSV_NET_TRACE=file`: per-frame trace of the battle socket and inputs (`zdcheck.py`, `recvcheck.py`,
    `cmptrace.py`).
  - `ZDXSV_PW_HASH=1`: per-frame hash of each player's work RAM, written to the net trace (`pwcheck.py`).
  - `ZDXSV_PW_DUMP=file`: player work RAM of every saved frame (`pwdiff.py`).
  - `ZDXSV_RAM_DUMP=dir,start,step,count`: EE RAM (32 MB) to `dir/<frame>.bin` (`ramcount.py`, `ramvals.py`).
  - `ZDXSV_SNAP=dir,n`: GS screenshot `dir/v<vsync>.png` every n vsyncs (needs a renderer; not in headless runs).

### Game investigation (EE recompiler)
- `ZDXSV_EE_PROBE=pc,pc,..` (hex): each time the EE reaches one of these PCs, log registers and memory to
  `ZDXSV_EE_PROBE_OUT-<n>.txt` (default prefix `eeprobe`).
  - `ZDXSV_EE_PROBE_MEM=addr` (hex, 48 bytes; default 0xc22c98) or `sp` (128 bytes from the stack pointer).
- `ZDXSV_EE_WATCH=addr[:len],..` (hex): log every EE store into these ranges (pc, address, value, ra, stack, GGPO
  frame) to `ZDXSV_EE_PROBE_OUT-w<n>.txt`.
- `ZDXSV_EE_CLAMP=addr,max[,lo,hi][;..]` (hex): keep a u16 at addr at or below max (only while in lo..hi). Used to
  shorten the 出撃準備 timer (`rbk.sh`).
