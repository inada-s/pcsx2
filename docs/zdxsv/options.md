# Options

Every option this fork adds. Features are described in the topic documents;
this file is the reference for names, values and defaults.

All options except the one setting below are environment variables, read once
at startup. None is needed to play.

The **Use** column says what an option is for:

| Use | Meaning |
|---|---|
| feature | turns a feature on or off |
| tuning | changes a value of a feature that is on |
| test | drives a test: fixed inputs, scripted actions, exit at the end |
| control | makes a test fail on purpose, to show the test detects the problem |
| diagnostic | writes traces, dumps or screenshots |

## Test options

Every build reads every option: a released build must be able to take part in
the rig tests, as a peer of a newer build.

## Game check

The fork's hooks patch fixed addresses of one game build: serial `SLPS-25419`
with ELF CRC `435D8236`. With any other disc (another game, or a patched or
other build of this one) everything is off in every build: GGPO, replays, the
recompiler hooks and the platform info. `ZDXSV_GGPO` or `ZDXSV_REPLAY` then
only log `ZdxsvGgpo: off: not the Z game (serial .. CRC ..)`. A lobby-style
server of another game gets no platform info
(`DEV9: TCP: zdxsv lobby question, but not the Z game`).

## Setting in PCSX2.ini

| Setting | Default | Use | Meaning |
|---|---|---|---|
| `[DEV9/Eth] ZdxsvGgpo` | `true` | feature | Lobby battles of the game run over GGPO (`ZDXSV_GGPO` options `net=1,lobby=1`). Read when the game starts; Settings → Network & HDD. Other games are never affected. |
| `[EmuCore/GS] ZdxsvLowLatencyVsync` | `true` | feature | Present the finished frame before the frame limiter sleeps, and poll input right before the next frame runs. Settings → Network & HDD. Other games are never affected. |
| `[DEV9/Eth] ZdxsvLiveAutoNext` | `false` | feature | Live spectating: when a stream ends, watch the lobby's next live battle ([replay.md](replay.md), Live spectating). Settings → Network & HDD. |

## Network

| Option | Default | Use | Meaning |
|---|---|---|---|
| `ZDXSV_PLATFORM_INFO=0` | on | test | Send no platform info. The server sees a real PS2. |
| `ZDXSV_GAME_CRC=hex` | `435D8236` | control | The CRC taken as the Z game's (Game check). Another value runs the Z game as a foreign game: everything off. |
| `ZDXSV_STUN_PORT=port` | 8201 | tuning | STUN port of the lobby. Asked only with `ZDXSV_GGPO` `lobby=1`. The connectivity test also uses port + 1; a server without that socket gives `nat=unknown`. |
| `ZDXSV_LOBBY_STATE=1` | off | feature | Save states made online keep working after a load. See [lobby.md](lobby.md). |
| `ZDXSV_UPDATE_URL=url` | GitHub releases of this fork | test | The updater reads its release list from this URL. |

## ZDXSV_GGPO

`ZDXSV_GGPO="key=value,..."` starts a GGPO session in a running game. It
replaces the options of the `ZdxsvGgpo` setting; `ZDXSV_GGPO=0` turns GGPO
off. Any other value turns it on. Without `net=1` the session is a synctest. Log lines start with
`ZdxsvGgpo`. See [rollback.md](rollback.md) and [lobby.md](lobby.md).

### Synctest keys

| Key | Default | Use | Meaning |
|---|---|---|---|
| `start=N` | 1500 | test | Vsync, counted from boot, at which the session starts. |
| `frames=N` | 3000 | test | Frames the session runs. Then it is closed and reported. |
| `check=N` | 6 | test | Check distance, 1 to 6: every N frames GGPO loads the state N frames back, reruns and compares state hashes. |
| `seed=N` | 1 | test | Seed of the random pad input (both pads, a new input every 5 frames). |
| `input=host` | random | test | Pad 1 comes from the host pad; pad 2 stays random. |
| `input=none` | random | test | No buttons, sticks centered. |
| `mask=hex` | `ffff` | test | Random buttons are limited to these bits. `fcff` leaves out Select and Start. |
| `hash=pw` | `pw` | test | What the synctest compares. `pw`: the player work of all 4 players (with machine-local fields masked) and the 2 game RNG words. `pos`: x, y, z of the 4 players and the RNG words. `full`: EE RAM and delta state; a rerun's IOP/event cycles differ (code cache), so it always reports mismatches. |
| `sync=0` | 1 | test | No state hashes. Always the case with `net=1`. |
| `control=input` | off | control | Reruns get other inputs. The synctest must report mismatches. |

### Battle keys (`net=1`)

| Key | Default | Use | Meaning |
|---|---|---|---|
| `net=1` | off | feature | A battle between peers instead of a synctest. It starts when the game opens its battle socket. |
| `players=N` | 4 | test | Number of peers, 2 to 4. GGPO player = battle position + 1. A lobby battle takes it from the battle info. |
| `port=P` | 7001 | tuning | UDP port. Position p listens on P + p. With `lobby=1` the emulator listens on P itself. |
| `host=H` | 127.0.0.1 | test | Address of the peers. Not used with `lobby=1`. |
| `relay=R` | off | test | Remote position p is at port R + 8 * own position + p, so each pair can go through its own `udprelay.py`. |
| `delay=N` | 0 | tuning | Fixed GGPO input delay in frames. Without it a lobby battle picks the delay from the ping test. |
| `mindelay=N` | 2 | tuning | Lower bound of the picked delay. |
| `lobby=1` | off | feature | Battles come from the zdxsv lobby. See [lobby.md](lobby.md). |
| `osd=0` | 1 | feature | Hide the network status OSD. |
| `replay=DIR` | see meaning | feature | Save every battle to `DIR`. Lobby battles save to `<data dir>/replays` without it. `replay=0` turns saving off. |
| `badsession=1` | off | control | The ping test of this client uses another session id, so no peer answers it and every client cuts the battle connection. |
| `advertise=P` | off | test | The platform info announces only `127.0.0.1:P`, so peers reach this client through a local `udprelay.py` at P that adds latency. |

## Rollback tests

| Option | Default | Use | Meaning |
|---|---|---|---|
| `ZDXSV_RBK=i/N` | off | test | Start from a post-entry save state as battle position i (0-based) of N. The emulator answers the lobby itself, runs turbo until the battle, and exits when the GGPO session ends. |
| `ZDXSV_RBK_TIME=s` | 210 | test | Time limit of the battle in seconds. |
| `ZDXSV_RBK_COUNT=n` | 0 | test | Number of battles. 0 = rematch by input. |
| `ZDXSV_RBK_GAUGE=v` | 600 | test | Force gauge (戦力ゲージ) of both sides. |
| `ZDXSV_RBK_TURBO=1` | off | test | The battle runs turbo too. |
| `ZDXSV_RAND_INPUT=seed` | off | test | Seeded random pad input. |
| `ZDXSV_ZDS_PS=1` | off | feature | Play-start barrier: every peer starts the battle on the same GGPO frame. With it off, `rbk.sh 4 1` still passed `pwcheck.py` (coordinates and RNG B equal after the battle load), so it is not a desync control for that check. |
| `ZDXSV_K3_LAG=n` | 8 | tuning | GGPO frames between the game sending a round-handshake message and the input that carries it. `0` is a control: the battle then depends on network timing. |
| `ZDXSV_SAVE_ALL=1` | off | control | Delta-save every GGPO frame, also frames that can no longer be rolled back. |
| `ZDXSV_NET_TAIL=n` | 300 | tuning | Frames run after the battle end message before the session stops. |
| `ZDXSV_NET_DISCONNECT_MS=ms` | 5000 | tuning | GGPO disconnect timeout. |
| `ZDXSV_VM_TEST=frame:shutdown` or `frame:reset` | off | test | Shut down or reset the VM once a session or replay reaches that GGPO frame. |

## Replay playback

See [replay.md](replay.md).

| Option | Default | Use | Meaning |
|---|---|---|---|
| `ZDXSV_REPLAY=file.pb` | off | feature | Play a saved replay. `a.pb;b.pb` loads the files that different players saved of one battle. |
| `ZDXSV_REPLAY=udp://host:port[/code]` | off | feature | Watch a lobby battle live through the lobby (its UDP port; no code = the newest live battle). See `replay.md` Live spectating. |
| `ZDXSV_REPLAY_POV=P` | position of the first file | feature | Start from the point of view of position P. |
| `ZDXSV_REPLAY_BAR=1` / `=0` | automatic | feature | Always show the control bar, or never. |
| `ZDXSV_REPLAY_KEY_DISPLAY=1` | off | feature | Start with the key display on. |
| `ZDXSV_REPLAY_SKIP_MS=0` | on | feature | Do not skip the mobile suit selection. |
| `ZDXSV_REPLAY_KEY=n` | 600 | tuning | Interval of the seek keys in frames. 0 = none: no backward seek. |
| `ZDXSV_REPLAY_TURBO=1` | off | test | Play turbo. |
| `ZDXSV_REPLAY_EXIT=1` | off | test | Exit at the end instead of pausing. |
| `ZDXSV_LIVE_NEXT=N` | the setting | test | Live auto-next for N more battles (0 = off), over `ZdxsvLiveAutoNext`. |
| `ZDXSV_REPLAY_SEEK=at:to[,at:to...]` | off | test | Seek to frame `to` when frame `at` is reached. |
| `ZDXSV_REPLAY_POV_AT=frame:P[,...]` | off | test | Switch to position P when that frame is reached. |
| `ZDXSV_REPLAY_ROUND_AT=frame:N[,...]` | off | test | Jump to round N (0 = the briefing) when that frame is reached. |
| `ZDXSV_REPLAY_TAKEOVER=frame[:src]` | off | test | Take over at that frame with no input matching. No src: the host pad plays; `replay` = the file's own input `delay` frames ahead (the replay must play unchanged); `rand` = random buttons (a control: differs from the replay soon after the frame). |
| `ZDXSV_REPLAY_TAKEOVER_RETRY=frame` | off | test | While taken over, retry from the takeover frame when that frame is reached. |
| `ZDXSV_REPLAY_FOUR=1` | off | feature | Four-screen: one more PCSX2 per other position with a file, 2x2, held on one frame (SpectateSync.h). |
| `ZDXSV_REPLAY_GROUP=id` | - | internal | Set by the four-screen host for its guests (with `ZDXSV_REPLAY_POV`). |
| `ZDXSV_REPLAY_SYNC=0` | on | test | Four-screen control: members publish their frame but neither wait nor seek to the newest. |
| `ZDXSV_REPLAY_KEY_NOHLE=1` | off | control | Seek keys restore no battle-socket state. The replay then drifts. |

## Delta state self-test

`ZDXSV_DELTA_TEST="key=value,..."` (any value turns it on): from vsync `start`,
save every frame; every `every` frames load the frame `depth` back and rerun.
The 2nd rerun of a window must hash like the 1st: both start from the same load with the same
recompiler code cache. The 1st rerun can differ from the first run: the recompilers end a block
where the next PC is already compiled, so events land at other cycles. Log lines start with
`ZdxsvDelta`.

| Key or option | Default | Use | Meaning |
|---|---|---|---|
| `start=N` | 3000 | test | Vsync of the first save. |
| `frames=N` | 1800 | test | Frames the test runs. |
| `depth=N` | 8 | test | Frames each rollback goes back. |
| `every=N` | 20 | test | Frames between rollbacks. |
| `replays=N` | 2 | test | Reruns of each window, each compared with the one before. The result (PASS/FAIL) counts reruns 2 and later; 1 = no result. |
| `gap=N` | 0 | test | The N frames before each rollback window are not saved; older saves are still discarded. This is the save skip GGPO does for confirmed frames. A gap above `depth` drops every save before the window. At most `every - depth - 1`. |
| `break=ee` | off | control | A load does not restore EE RAM. Mismatches must be reported. |
| `ZDXSV_DELTA_HOT=0` | on | control | Every written page is write-protected each frame; no hot-page copy. |

## Input latency measurement

`ZDXSV_INPUT_LATENCY="key=value,..."` (any value, even empty, turns it on):
presses a button on pad 1 and logs the time of each stage: press, host poll,
game pad read, RAM change, present. Log lines start with `ZdxsvLatency`.

| Key | Default | Use | Meaning |
|---|---|---|---|
| `btn=name` | `down` | test | Button of the even presses: `up`, `down`, `left`, `right`, `circle`, `cross`, `triangle`, `square`, `start`, `select`. |
| `back=name` | `up` | test | Button of the odd presses, so a cursor returns. `none` = always `btn`. |
| `addr=0x..` | search mode | test | EE address of the byte the press changes. Without it, search mode finds that byte. |
| `held=1` | off | test | Search for bytes that differ only while the button is held (a pad buffer), not bytes that stay changed (a cursor). |
| `count=N` | 20 | test | Number of presses. |
| `start=N` | 600 | test | Vsync of the first press. |
| `go=path` | off | test | Instead of `start`: the first press is 60 vsyncs after this file appears. |
| `hold=N` | 4 | test | Frames each press is held. |
| `gap=N` | 60 | test | Frames from one press to the next. At least `hold` + 20. |
| `seed=N` | 1 | test | Seed of the press time inside the frame before its poll. |
| `out=path` | off | diagnostic | Also write one CSV line per press to this file. |

## Traces and dumps

| Option | Use | Meaning |
|---|---|---|
| `ZDXSV_NET_TRACE=file` | diagnostic | Per-frame trace of the battle socket and the inputs. Read by `tools/zdxsv/zdcheck.py`, `tools/zdxsv/recvcheck.py`. |
| `ZDXSV_PW_HASH=1` | diagnostic | The sync check: per frame, a hash of each player's x, y, z and the 2 game RNG words, written to the net trace. `pwcheck.py` compares the coordinates and RNG B (`0x6d793c`); RNG A (`0x6d7940`) also takes machine-local sound draws, so it is only reported. |
| `ZDXSV_PW_DUMP=file` | diagnostic | Work RAM of the players for every saved frame. Read by `pwdiff.py`. |
| `ZDXSV_RAM_DUMP=dir,start,step,count` | diagnostic | EE RAM (32 MB) to `dir/<frame>.bin`. Read by `tools/zdxsv/ramcount.py`, `tools/zdxsv/ramvals.py`. |
| `ZDXSV_SNAP=dir,n` | diagnostic | GS screenshot `dir/v<vsync>.png` every n vsyncs. Needs a renderer; not in headless runs. |

## Game investigation (EE recompiler)

Addresses and PCs are hexadecimal.

| Option | Use | Meaning |
|---|---|---|
| `ZDXSV_EE_PROBE=pc,pc,..` | diagnostic | Each time the EE reaches one of these PCs, log registers and memory to `<prefix>-<n>.txt`. |
| `ZDXSV_EE_PROBE_OUT=prefix` | diagnostic | Prefix of the probe and watch files. Default `eeprobe`. |
| `ZDXSV_EE_PROBE_MEM=addr` | diagnostic | Memory a probe logs: 48 bytes at `addr` (default `0xc22c98`), or `sp` for 128 bytes from the stack pointer. |
| `ZDXSV_EE_WATCH=addr[:len],..` | diagnostic | Log every EE store into these ranges (pc, address, value, ra, stack, GGPO frame) to `<prefix>-w<n>.txt`. |
| `ZDXSV_EE_CLAMP=addr,max[,lo,hi][;..]` | test | Keep a u16 at `addr` at or below `max`, only while it is in `lo..hi`. Used to shorten the sortie preparation (出撃準備) timer in rollback tests. A replay of such a battle needs the same value. |
