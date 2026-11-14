# Options

Every option this fork adds. Features are described in the topic documents;
this file is the reference for names, values and defaults.

All options except the settings below are environment variables, read once
at startup. None is needed to play.

The **Use** column says what an option is for:

| Use | Meaning |
|---|---|
| feature | turns a feature on or off |
| tuning | changes a value of a feature that is on |
| test | drives a test: fixed inputs, scripted actions, exit at the end |
| control | makes a test fail on purpose, to show the test detects the problem |
| diagnostic | writes traces, dumps or screenshots |

Every build reads every option, so a released build can be a peer in the rig
tests.

## Build options

CMake cache variables. Only the official release build (`zdxsv_release.yml`,
tags `zdxsv-X.Y.Z`) sets them from the repository variables of the same name
([tools](tools.md)); a build from source connects to a zdxsv on this machine
(zdxsv `scripts/run-local.sh`). `ZDXSV_SERVER_HOST` and `ZDXSV_API_URL` are also
environment variables (test): set, they replace the build's value.

| Option | Default | Meaning |
|---|---|---|
| `ZDXSV_SERVER_HOST` | `127.0.0.1` | The game server host. The Z game with the extensions on goes online without DEV9 (with or without `EthEnable`): its net RPCs are answered on the EE, network setup from fixed replies, every name resolves to this host (host DNS, 5 s wait), TCP on host sockets (4 slots), only to port 8200 (the lobby); any other connect fails (no DNAS, no login pages, no battle server: needs `ZdxsvFastLogin`, and GGPO for battles). Empty: the DEV9 path, as upstream (the test rigs' `DEV9=1`). Release: the repository variable. |
| `ZDXSV_API_URL` | `https://<ZDXSV_SERVER_HOST>:8443`; without a host `http://127.0.0.1:8203` (a local zdxsv's `ZDXSV_LOBBY_API_ADDR`) | The zdxsv public API, the default of the API URLs below (`server` + path in their Default column). Empty: the API URLs are empty. |
| `ZDXSV_UDPTEST_URL` | empty | Default of `ZdxsvUdpTestUrl`. Release: the repository variable. |
| `ZDXSV_SENTRY_DSN` | empty | Where crash reports go. Empty: no crash reports and no setting. Release: the repository variable. |

## Game check

Everything is on only for serial `SLPS-25419` with ELF CRC `435D8236`: the
hooks patch fixed addresses of that build. `SLPS-25305` with ELF CRC
`992356FE` gets the same GGPO, replay, settings sync, fast boot, fast login,
no DEV9 and platform info defaults; single draw list, the ally HP display and
the game patches are SLPS-25419 only. With any
other disc GGPO, replays, the recompiler hooks and the platform info are off,
and `ZDXSV_GGPO` or `ZDXSV_REPLAY` only log `ZdxsvGgpo: off: not the Z game`.

| Option | Default | Use | Meaning |
|---|---|---|---|
| `ZDXSV_EXTENSIONS=0` | on | feature | Turns off every zdxsv online battle extension: the Z game runs as any other game (above): no GGPO, replays, recompiler hooks, platform info (so no connectivity test, UPnP or HTTPS latency test), low-latency present, enforced emulation settings or the quiet missing patches.zip. No item on the settings page. Logs `ZdxsvGgpo: off: ZDXSV_EXTENSIONS=0`. It does not change the DNS name of the zdxsv server or TLS to it. |

## Settings in PCSX2.ini

All but `ZdxsvReplayUpload`, `ZdxsvReplayUploadUrl`, `ZdxsvUdpTestUrl`, `ZdxsvReplayStateUrl`, `ZdxsvLobbyApiUrl` (ini only) and `ZdxsvReplayFavorites` (set in the replay
window) are on the page Settings → zdxsv (global or per game). An environment variable named in a row overrides the setting; the
page greys it out and its tooltip names the variable.

| Setting | Default | Use | Meaning |
|---|---|---|---|
| `[DEV9/Eth] ZdxsvGgpoMinDelay` | `2` | feature | Lowest GGPO input delay (frames, 2..6) a lobby battle picks, and the replay takeover delay. Read when the game starts. `mindelay=` overrides it. |
| `[DEV9/Eth] ZdxsvRollbackBudget` | `1.0` | internal | Rollback budget learned per PC (frames, 0..1.5), saved after each long net battle (`rollback.md`). |
| `[DEV9/Eth] ZdxsvGgpoPort` | `0` | feature | UDP port of lobby battles (ping test and GGPO). 0 or out of range: a random port in 29700..29800, picked when the game starts with `lobby=1` and saved to the setting. `port=` overrides it. |
| `[EmuCore/GS] VsyncEnable` | `true` | feature | Upstream VSync, on by default in this build (upstream: off); Settings → Emulation. |
| `[DEV9/Eth] ZdxsvLiveAutoNext` | `false` | feature | Live spectating: when a stream ends, watch the lobby's next live battle. `ZDXSV_LIVE_NEXT` overrides it. |
| `[DEV9/Eth] ZdxsvUPnP` | `true` | feature | When the connectivity test finds the GGPO port not open, map it on the router with UPnP and test again (`lobby.md`). |
| `[DEV9/Eth] ZdxsvFeatureLimits` | `true` | feature | On: the upstream features zdxsv limits are off (`docs/zdxsv/features.md`, `FeatureLimits`). Off: as upstream. Ini only. |
| `[DEV9/Eth] ZdxsvNetOsd` | `true` | feature | Network status OSD in GGPO battles. Read when the game starts. `osd=` overrides it. Off: shown only while an opponent is interrupted or disconnected. |
| `[DEV9/Eth] ZdxsvFrameOsd` | `false` | feature | Frame time OSD in GGPO battles and replays (docs/zdxsv/rollback.md). Read when the game starts. `ZDXSV_FRAME_OSD` overrides it. |
| `[DEV9/Eth] ZdxsvAllyHp` | `true` | feature | In battle, the own HP display shows the teammate's HP in place of the own max HP. Applied when settings change. `ZDXSV_ALLY_HP` overrides it. |
| `[DEV9/Eth] ZdxsvStickMessageButton` | `Start` | feature | While this pad button is held, the D-pad and the left stick also tilt the right stick (the team messages); the real right stick still works. `None` (Off), `Start`, `Select`, `L1`, `L2`, `R1`, `R2`, `L3`, `R3`, `Triangle`, `Circle`, `Cross`, `Square`. The Z game only. Applied when settings change. |
| `[UI] ZdxsvWheelVolume` | `true` | feature | The mouse wheel over the game screen changes the volume (5% a notch, not saved) with an OSD. Off: the wheel is input, as upstream. Ini only. |
| `[DEV9/Eth] ZdxsvCrashReports` | `true` | feature | Send crash reports ([crash-reports.md](crash-reports.md)), in a build with a DSN. Settings → zdxsv. |
| `[DEV9/Eth] ZdxsvReplaySkipMs` | `true` | feature | A replay plays its rounds only: it skips the mobile suit selections and the rematch screens (a file without round frames: the first mobile suit selection). `ZDXSV_REPLAY_SKIP_MS` overrides it. |
| `[DEV9/Eth] ZdxsvReplayKeyDisplay` | `true` | feature | Replays start with the key display on. `ZDXSV_REPLAY_KEY_DISPLAY` overrides it. |
| `[DEV9/Eth] ZdxsvReplayStateUrl` | the hosted `rbk-p1.p2s` | feature | The post-entry state a replay or live battle starts from: URL (downloaded once into the cache folder) or file; empty = the booted state. `ZDXSV_REPLAY_STATE` overrides it. |
| `[DEV9/Eth] ZdxsvFastBoot` | `true` | feature | Z game and SLPS-25305, from boot: unlimited speed and automatic presses through the title, main menu and network menus (broadband, network setting 1 with the DEV9 adapter on; the terms screen skipped) to the lobby user_id selection (without fast login: to the login browser), then normal speed. Stops at once at the NET file load when the memory card is unformatted or has no NET file, at the lobby when the account has no user_id, and when fast login goes back to the network menu (the player connects again). Settings → zdxsv → Fast boot. Env `ZDXSV_FAST_BOOT=1/0` overrides. |
| `[DEV9/Eth] ZdxsvFastBootState` | `true` | feature | With `ZdxsvFastBoot`: the first boot saves a state at the NET file load (`sstates/<serial> zdxsv-fastboot <state version> <memory card checksum>.p2s`; `zdxsv-fastboot-eth` on the DEV9 path with the adapter on, `zdxsv-fastboot-noeth` with it off, as each stalls on the others); later boots with the same memory card 1 contents load it and skip the title and main menu. The state holds the system data the game auto-loaded from the card, so another card, or one written since, boots in full and replaces the state. Delete the file to retake it. Env `ZDXSV_FAST_BOOT_STATE=1/0` overrides. |
| `[DEV9/Eth] ZdxsvFastBootLobby` | `false` | test | With `ZdxsvFastBoot`: at the user_id selection, presses Circle (still at unlimited speed) until ID 1 is picked and confirmed, and ends in the lobby (戦場選択, emulog `Zdxsv: fast boot: lobby`); `gave up in the user_id selection` after 60 s. No settings UI. Env `ZDXSV_FAST_BOOT_LOBBY=1/0` overrides. |
| `[DEV9/Eth] ZdxsvFastLogin` | `true` | feature | Z game and SLPS-25305, on the way to the lobby: when DNAS starts, log in through `ZdxsvLoginApiUrl` with the token in `inis/zdxsv_auth_token.txt` (forms `token` and `game`: the game code); no token: the VM pauses, the sign-in of `ZdxsvAuthUrl` runs in the browser, the VM resumes when it ends (resuming by hand cancels it) and the new token logs in; at every DNAS start, and jump to the lobby connect (no DNAS, no login browser). No token after the sign-in, a refused login, or resuming during the hold: back to the network menu (connection menu), no DNAS. No `ZdxsvLoginApiUrl`: the normal login. `ZDXSV_FAST_LOGIN` overrides. Ini only. |
| `[DEV9/Eth] ZdxsvLoginApiUrl` | server `/lbs/login` | feature | The zdxsv login API (`POST`; the zdxsv lobby API `/lbs/login`) (form `token`; JSON `login_key`, `session_id`, `lobby_addr`, `lbs_info`) used by `ZdxsvFastLogin`. The same with or without DEV9. `ZDXSV_LOGIN_API_URL` overrides it (the rigs: the local stack). Ini only. |
| `[DEV9/Eth] ZdxsvAuthUrl` | server `/auth` | feature | The zdxsv sign-in base URL for Settings → zdxsv → Account: the browser opens `<url>/start` (the reply comes to a listener on `127.0.0.1`, random port, for 10 minutes); `POST <url>/token` (PKCE) saves the token to `inis/zdxsv_auth_token.txt`, the token `ZdxsvFastLogin` sends; Sign Out `POST <url>/revoke` and deletes the file. `ZDXSV_AUTH_URL` overrides it. Ini only. |
| `[DEV9/Eth] ZdxsvAccount` | `0` | feature | Which account of the signed-in Discord account the fast login uses (form `account`, from 1); `0`: the server's default (the first; with no account, the login makes one); `-1`: an empty slot, the login sends `account=new`, the server makes the account and the setting becomes its number. Settings → zdxsv → Account sets it from `POST <ZdxsvAuthUrl>/accounts`; Carry Over from PS2 (`POST <ZdxsvAuthUrl>/accounts/carry`, form `code`) picks the carried account; sign-in and sign-out reset it to `0`. |
| `[DEV9/Eth] ZdxsvLobbyApiUrl` | server | feature | The lobby's public API (`http://host:port`, zdxsv `ZDXSV_LOBBY_API_ADDR`) the replay window's Remote (`/lbs/replay`) and Live (`/lbs/live`) tabs and the Favorites tab ask; empty = none. A live battle is watched at `live:<this URL>#<code>`. |
| `[DEV9/Eth] ZdxsvReplayFavorites` | empty | feature | The replay window's Favorites: one value per battle (battle code, start time, players and rounds, tab-separated), at most 1000; added from the Remote tab, removed in the Favorites tab. |
| `[DEV9/Eth] ZdxsvReplayUpload` | `true` | feature | Post each lobby battle's `.pb` to `ZdxsvReplayUploadUrl` (on by default). Only when the game's hosts resolve to the build's `ZDXSV_SERVER_HOST` (no-DEV9, the host not given by the environment): a battle on the DEV9 path or on a host from the environment posts nowhere. `upload=` overrides it. |
| `[DEV9/Eth] ZdxsvReplayUploadUrl` | server `/lbs/replay_upload` | feature | The lobby's replay upload API (zdxsv `/lbs/replay_upload`); empty = no upload. |
| `[DEV9/Eth] ZdxsvSaveReplay` | `true` | feature | Save each lobby battle's `.pb` to `ZdxsvReplayPath`. `replay=` in `ZDXSV_GGPO` overrides it. |
| `[DEV9/Eth] ZdxsvReplayPath` | `<data dir>/replays` | feature | Folder lobby battles save to and the Local replay list reads; relative to the data dir. A folder that cannot be created: the default. |
| `[DEV9/Eth] ZdxsvUdpTestUrl` | `ZDXSV_UDPTEST_URL` (build) | feature | The udptest function (zdxsv `infra/udptest`) the connectivity test asks for a `Hello` from another network before it reports `open`; empty = not asked. `ZDXSV_UDPTEST_URL` overrides it. |

## Network

| Option | Default | Use | Meaning |
|---|---|---|---|
| `ZDXSV_PLATFORM_INFO=0` | on | test | Send no platform info. The server sees a real PS2. |
| `ZDXSV_GAME_CRC=hex` | `435D8236` | control | The CRC taken as the Z game's (Game check). Another value runs the Z game as a foreign game: everything off. |
| `ZDXSV_UPNP_ANY_IGD=1` | off | test | UPnP also uses a router whose IGD reports no routable WAN address (`0.0.0.0`); see [lobby.md](lobby.md). |
| `ZDXSV_STUN_PORT=port` | 8221 | tuning | STUN port of the lobby. Asked only with `ZDXSV_GGPO` `lobby=1`. The connectivity test also uses port + 1; a server without that socket gives `nat=unknown`. |
| `ZDXSV_NAT_STUN=host:port,host:port` | `stun.l.google.com:19302,stun.cloudflare.com:3478` | test | RFC 5389 STUN servers the connectivity test asks when the lobby finds the port closed: different mappings there = `nat=symmetric`. `none` = not asked. |
| `ZDXSV_HTTPS_LATENCY=0` | on | test | No HTTPS latency test, no `<region>=ms` platform info. `=name:host,...` measures these hosts (`https://host/api/ping`) instead of the regions. See [lobby.md](lobby.md). |
| `ZDXSV_TLS=0` | on when the client has pins | test | TCP to the zdxsv server without TLS (a server without TLS ports). See [lobby.md](lobby.md). |
| `ZDXSV_TLS_TEST_PIN=1` | off | test | Trusts the zdxsv test CA (`pkg/tlsconf/testdata` in the zdxsv repository), which also turns TLS on. See [lobby.md](lobby.md). |
| `ZDXSV_PAUSE_LINE_CHECK=0` | on | control | While the VM is paused, leave the lobby's line checks (0x6001) to the game: a pause over 2 min drops the connection. |
| `ZDXSV_UPDATE_URL=url` | GitHub releases of this fork | test | The updater reads its release list from this URL. |
| `ZDXSV_SENTRY_DSN=dsn` | the build's DSN | test | Crash reports go to this DSN, with the environment `development`. |
| `ZDXSV_CRASH_REPORT_TEST=1` | off | test | In a build with a DSN: starts crash reports whatever `ZdxsvCrashReports` says, without changing it, and crashes PCSX2 right after. |

## ZDXSV_GGPO

`ZDXSV_GGPO="key=value,..."` starts a GGPO session in a running game. It
replaces the default options (GGPO is on by default); `ZDXSV_GGPO=0` (test)
turns GGPO off and leaves the other extensions on. Any other value turns it on. Without `net=1` the session is a synctest. Log lines start with
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
| `hash=pw` | `pw` | test | What the synctest compares. `pw`: the 4 players' work (machine-local fields masked) and the 2 game RNG words. `pos`: x, y, z of the 4 players and the RNG words. `full`: EE RAM and delta state; always reports mismatches (rerun timing differs). |
| `sync=0` | 1 | test | No state hashes. Always the case with `net=1`. |
| `control=input` | off | control | Reruns get other inputs. The synctest must report mismatches. |

### Battle keys (`net=1`)

| Key | Default | Use | Meaning |
|---|---|---|---|
| `net=1` | off | feature | A battle between peers instead of a synctest. It starts when the game opens its battle socket. |
| `players=N` | 4 | test | Number of peers, 2 to 4. GGPO player = battle position + 1. A lobby battle takes it from the battle info. |
| `port=P` | 7001; with `lobby=1` the setting `ZdxsvGgpoPort` | tuning | UDP port. Position p listens on P + p. With `lobby=1` the emulator listens on P itself. |
| `host=H` | 127.0.0.1 | test | Address of the peers. Not used with `lobby=1`. |
| `relay=R` | off | test | Remote position p is at port R + 8 * own position + p, so each pair can go through its own `udprelay.py`. |
| `delay=N` | 0 | tuning | Fixed GGPO input delay in frames. Without it a lobby battle picks the delay from the ping test. |
| `mindelay=N` | the setting | tuning | Lower bound of the picked delay and the replay takeover delay. Overrides `ZdxsvGgpoMinDelay`. |
| `lobby=1` | off | feature | Battles come from the zdxsv lobby. See [lobby.md](lobby.md). |
| `osd=0` / `osd=1` | the setting | feature | Hide or show the network status OSD, over `ZdxsvNetOsd`. |
| `replay=DIR` | see meaning | feature | Save every battle to `DIR`. Lobby battles save to the folder of `ZdxsvSaveReplay` / `ZdxsvReplayPath` without it. `replay=0` turns saving off (upload and live streaming still run). |
| `upload=URL` | the setting | test | Replay upload API, over `ZdxsvReplayUpload` and `ZdxsvReplayUploadUrl`, also with a host entry. |
| `live=URL` | `ZdxsvLobbyApiUrl` | test | Lobby API the live uplink posts to, also with a host entry (without it, only a server found by DNS gets the stream). |
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
| `ZDXSV_K3_LAG=n` | 8 | tuning | GGPO frames between the game sending a round-handshake message and the input that carries it. `0` is a control: the battle then depends on network timing. |
| `ZDXSV_RERUN_REVERB=1` | off | control | SPU2 reverb runs in rerun frames. Default: skipped (their sound is dropped) unless an SPU2 IRQ address lies in the reverb work area. |
| `ZDXSV_RERUN_VU1=1` | off | control | VU1 microprograms run in every rerun frame. Default: only in the last rerun frame of a rollback, whose drawing the first present after the rollback shows; none with the single draw list. |
| `ZDXSV_RERUN_TAIL=n` | 2 (0 with the single draw list) | control | The last n rerun frames of a rollback run the game's render callbacks, the last n - 1 VU1 microprograms (n with the single draw list). Without the single draw list, below the default the picture after a rollback breaks (`tests/zdxsv/rerunpic.py` FAIL control). |
| `ZDXSV_SINGLE_DRAW_LIST=0` | on | control | Z game: no single draw list (the game draws a frame's draw list in the next frame, as stock). See rollback.md. |
| `ZDXSV_LOW_LATENCY_PRESENT=0` | on | control | Z game: no low-latency present. On: present the finished frame before the frame limiter sleeps, and poll input right before the next frame runs (in a net battle: before the sleep, see [rollback.md](rollback.md)). |
| `ZDXSV_AUTH_SIGNIN_TEST=1\|signout` | unset | test | At app start, `1` signs in once through `ZdxsvAuthUrl` and logs `Zdxsv: auth: open <url>` instead of opening a browser; `signout` signs out. |
| `ZDXSV_AUTH_URL=<url>` | unset | test | Overrides `ZdxsvAuthUrl`. |
| `ZDXSV_AUTH_AT_DNAS=0` | unset (on) | test | `ZdxsvFastLogin` with no token skips the sign-in at DNAS (back to the network menu); the rigs set it unless set. |
| `ZDXSV_AUTH_NO_BROWSER=1` | unset | test | Sign-in URLs are only logged (`Zdxsv: auth: open <url>`), no browser opens; the rigs set it, with `ZDXSV_AUTH_URL` = the local stack. |
| `ZDXSV_RERUN_EE_DRAW=1\|0` | SLPS-25305: 1; else 0 | control | `1`: the game's render callbacks run in every rerun frame, and nothing is kept (`ZDXSV_RERUN_KEEP`). `0`: not in rerun frames but the last two (see rollback.md). |
| `ZDXSV_RERUN_KEEP=0` or `=lo-hi,...` | bone slots, saber trails | control | EE RAM a rollback does not roll back during play: `0` none; else these ranges (hex, end exclusive) and the saber trails. Off with `ZDXSV_RERUN_EE_DRAW=1`. See rollback.md. |
| `ZDXSV_RERUN_PARTS=0` | on | control | With the bone slots kept: rerun frames that skip the render callbacks do not run the draws of parts attached to a player either (see rollback.md). |
| `ZDXSV_ROLLBACK_BUDGET=1` | off | test | The lobby delay subtracts the learned `ZdxsvRollbackBudget` instead of 0. |
| `ZDXSV_NET_TAIL=n` | 60 | tuning | Frames run after the battle end message is confirmed, before the session stops. |
| `ZDXSV_NET_DISCONNECT_MS=ms` | 10000 | tuning | GGPO disconnect timeout. |
| `GGPO_NETWORK_DELAY=ms` | 0 | test | GGPO holds each sent packet ms * (2/3 .. 1) before sending it ([rollback.md](rollback.md)). |
| `ZDXSV_NET_SESSION=s` | battle code | test | GGPO session code sent in the sync handshake instead of the battle code; a peer with another code is refused (`session mismatch`). |
| `ZDXSV_SYNC_FORCE=0` | forced | control | Sync-relevant settings are not forced (`Zdxsv/SyncSettings.cpp`); the log names those that would have been (`ZdxsvSync: ZDXSV_SYNC_FORCE=0, not forced:`). The fingerprint is sent anyway. |
| `ZDXSV_PINE_ONLINE=1` | unset (refused) | test | PINE answers while the Z game is online (lobby or net battle); the rigs set it unless set. |
| `ZDXSV_TEST_BREAKPOINT=<hex address>` | unset | test | The Z game: an EE breakpoint at that address at the first vsync, as if set in the debugger (the address must never run); going online clears it. |
| `ZDXSV_SYNC_SALT=s` | none | test | s is mixed into the sync fingerprint, as another build would be: a lobby battle with this client is cut. |
| `ZDXSV_DROP_SYNC=p` | none | test | The sync fingerprint of position p is left out of the received battle info, as a lobby that missed it: this client cuts the battle, its peers do not. |
| `ZDXSV_VM_TEST=frame:shutdown`, `frame:reset`, `frame:mute` or `frame:ui` | off | test | Shut down or reset the VM once a session or replay reaches that GGPO frame; `mute`: drop every GGPO packet sent and received from then on (the battle TCP stays up); `ui`: ask for Turbo, a frame advance, a slot 1 state load, a pause and the pause menu, as the hotkeys do. |

## Replay playback

See [replay.md](replay.md).

| Option | Default | Use | Meaning |
|---|---|---|---|
| `ZDXSV_REPLAY=file.pb` | off | feature | Play a saved replay. One file plays every position (a 2nd one is skipped). |
| `ZDXSV_REPLAY=live:<API URL>[#code]` | off | feature | Watch a lobby battle live through the lobby HTTP API (`http[s]://host:port`; no code = the newest live battle). See `replay.md` Live spectating. |
| `ZDXSV_REPLAY_POV=P` | the recorder's position | feature | Point of view P, picked before the start: the lobby answers name it as the own position. |
| `ZDXSV_REPLAY_STATE=url\|path` | the setting | feature | The post-entry state a replay starts from, over `ZdxsvReplayStateUrl`. |
| `ZDXSV_REPLAY_DRAW_SKIP=0` | on | diagnostic | Seek and skip frames draw (the game's draw callbacks and VU1 run); they are still not shown, heard or throttled. |
| `ZDXSV_REPLAY_BAR=1` / `=0` | off | diagnostic | Always show the replay control bar, or never (default: for 3 s after the mouse moves over the bottom quarter or a pad control). |
| `ZDXSV_REPLAY_KEY_DISPLAY=1` / `=0` | the setting | feature | Start with the key display on or off, over `ZdxsvReplayKeyDisplay`. |
| `ZDXSV_REPLAY_SKIP_MS=0` / `=1` | the setting | feature | Play everything / the rounds only, over `ZdxsvReplaySkipMs`. |
| `ZDXSV_FRAME_OSD=0` / `=1` | the setting | feature | Frame time OSD off / on, over `ZdxsvFrameOsd`; on, every 600 frames a `frame osd` log line with the stage means. |
| `ZDXSV_ALLY_HP=0` / `=1` | the setting | feature | Teammate HP on the HUD off / on, over `ZdxsvAllyHp`. |
| `ZDXSV_REPLAY_KEY=n` | 600 | tuning | Interval of the seek keys in frames. 0 = none: no backward seek. |
| `ZDXSV_REPLAY_TURBO=1` | off | test | Play turbo. |
| `ZDXSV_REPLAY_EXIT=1` | off | test | Exit at the end instead of pausing. |
| `ZDXSV_CYCLE_LOG=n` | off | test | Log `ZdxsvGgpo: cycles at frame f: ee .. iop ..` every n played frames. Two builds that differ in the CPU core must log the same values; the state hash does not catch an EE cycle shift. |
| `ZDXSV_EVENT_LOG=file[,lines[,from]]` | off | test | Replay playback: from the common state load, write one line per EE (`E`) and IOP (`I`) event test, `ee <cycle> <pc> iop <cycle> <pc>`, up to `lines` (2000000). Two builds that differ in the CPU core: `diff` the files, the first differing line is where the timing first splits. With `from` (rerun with the line number less a few), after `from` event lines also one `B ee <cycle> <pc>` line per EE recompiler (x86 and ARM64) block entry; `lines` counts these too. |
| `ZDXSV_LIVE_NEXT=N` | the setting | test | Live auto-next for N more battles (0 = off), over `ZdxsvLiveAutoNext`. |
| `ZDXSV_LIVE_PACING=0` | on | test | Live: no speed trim, only the wait for 60 frames at the newest one (control). |
| `ZDXSV_REPLAY_SEEK=at:to[,at:to...]` | off | test | Seek to frame `to` when frame `at` is reached. |
| `ZDXSV_REPLAY_POV_AT=frame:P[,...]` | off | test | Switch to position P when that frame is reached. |
| `ZDXSV_REPLAY_ROUND_AT=frame:N[,...]` | off | test | Jump to round N (0 = the briefing) when that frame is reached. |
| `ZDXSV_REPLAY_TAKEOVER=frame[:src]` | off | test | Take over at that frame with no input matching. No src: the host pad plays; `replay` = the file's own input `delay` frames ahead (the replay must play unchanged); `rand` = random buttons (a control: differs from the replay soon after the frame). |
| `ZDXSV_REPLAY_TAKEOVER_RETRY=frame` | off | test | While taken over, retry from the takeover frame when that frame is reached. |
| `ZDXSV_REPLAY_FOUR=1` | off | feature | Four-screen replay: one more PCSX2 per other position, 2x2, held on one frame. |
| `ZDXSV_REPLAY_GROUP=id` | - | internal | Set by the four-screen host for its guests (with `ZDXSV_REPLAY_POV`). |
| `ZDXSV_REPLAY_SYNC=0` | on | test | Four-screen control: members publish their frame but neither wait nor seek to the newest. |
| `ZDXSV_REPLAY_KEY_NOHLE=1` | off | control | Seek keys restore no battle-socket state. The replay then drifts. |

## Delta state self-test

`ZDXSV_DELTA_TEST="key=value,..."` (any value turns it on): from vsync `start`,
save every frame; every `every` frames load the frame `depth` back and rerun.
The 2nd rerun of a window must hash like the 1st. Log lines start with
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
| `break=host` | off | control | A load does not restore IOP and SPU2 RAM pages. Mismatches must be reported. |

## Input latency measurement

`ZDXSV_INPUT_LATENCY="key=value,..."` (any value, even empty, turns it on):
presses a button on pad 1 and logs the time of each stage: press, host poll,
game pad read, RAM change, present. Log lines start with `ZdxsvLatency`.
While presses run it also logs frame pacing per shown frame (present -> present,
and each frame-end interval split into push, limiter sleep, GGPO rollback, emulation)
and lists late presents (> 1.5x the mean) with their split. A rollback's rerun frames
are not shown, so they are no samples: their time (state load + reruns) is the
`rollback` stage of the shown frame they delay.

| Key | Default | Use | Meaning |
|---|---|---|---|
| `btn=name` | `down` | test | Button of the even presses: `up`, `down`, `left`, `right`, `circle`, `cross`, `triangle`, `square`, `start`, `select`. |
| `back=name` | `up` | test | Button of the odd presses, so a cursor returns. `none` = always `btn`. |
| `addr=0x..` | search mode | test | EE address of the byte the press changes. Without it, search mode finds that byte. |
| `held=1` | off | test | Search for bytes that differ only while the button is held (a pad buffer), not bytes that stay changed (a cursor). |
| `count=N` | 20 | test | Number of presses. 0 with `pixel=`: no presses, watch only: every 600 shown frames logs the vsyncs since the first shown frame, how many shown frames came right after a rollback and how many of each kind had no pixel change (a repeated picture). |
| `start=N` | 600 | test | Vsync of the first press. |
| `go=path` | off | test | Instead of `start`: the first press is 60 vsyncs after this file appears. |
| `hold=N` | 4 | test | Frames each press is held. |
| `gap=N` | 60 | test | Frames from one press to the next. At least `hold` + 20. |
| `seed=N` | 1 | test | Seed of the press time inside the frame before its poll. |
| `out=path` | off | diagnostic | Also write one CSV line per press to this file. |
| `pixel=x:y[;x:y]` | off | test | Watch these pixels (game frame coordinates, up to 16) of each shown frame (a one-row readback, nothing written) and log, per press, the vsync of the first change. |
| `tol=N` | 0 | test | With `pixel=`: a pixel changes only when a color channel differs from the shown frame before by more than N. For translucent HUD parts whose color moves by 1-2 every frame. |

## Traces and dumps

| Option | Use | Meaning |
|---|---|---|
| `ZDXSV_NET_TRACE=file` | diagnostic | Per-frame trace of the battle socket and the inputs; a `B <battle_code>` line at each GGPO session start and replay load. Read by `tools/zdxsv/zdcheck.py`, `tools/zdxsv/recvcheck.py`, `tools/zdxsv/pwcheck.py`. |
| `ZDXSV_PW_HASH=1` | diagnostic | Per frame, a hash of each player's x, y, z and the 2 game RNG words, written to the net trace (`H` lines). |
| `ZDXSV_PW_DUMP=file` | diagnostic | Work RAM of the players for every saved frame. Read by `pwdiff.py`. |
| `ZDXSV_RAM_DUMP=dir,start,step,count` | diagnostic | EE RAM (32 MB) to `dir/<frame>.bin`. Read by `tools/zdxsv/ramcount.py`, `tools/zdxsv/ramvals.py`. |
| `ZDXSV_AUDIO_WAV=file` | diagnostic | The samples the audio device reads, after the volume, to a 32-bit float WAV; `file.gaps.txt` lists each read more than 50 ms after the previous one (output frame, gap in ms): a resume of the output. Read by `tools/zdxsv/wavclicks.py`. |
| `ZDXSV_SNAP=dir,n[,from[,to]]` | diagnostic | GS screenshot `dir/v<vsync>.png` every n vsyncs, from vsync `from` up to (not including) `to`. Only shown frames: a request in a rollback goes to the next present. Needs a renderer; not in headless runs. |
| `ZDXSV_VU1_OPTEST=file[,n[,finite]]` | test | At the first VU1 micro program start, run each of 49 VU1 ops as a micro program through the VU1 core in use on `n` (default 2048) generated inputs, weighted to edges (denormals, zeros, exponent 255); `finite`: exponent 255 becomes 254 and denormals zero. Flags and pending Q/P are reset per case; VU1 is restored afterwards. Inputs and outputs (VF1-VF8, ACC, Q, P, status, MAC, clip) go to `file`; the log has one `VU1 optest: <op> <n> cases, hash <h>` line per op. Compare two builds with `tests/zdxsv/vu1_optest_cmp.py`. |
| `ZDXSV_EE_OPTEST=file[,n[,finite]]` | test | From vsync 300, one op per vsync: write a MIPS loop into EE RAM 0x01f00000 (1 MB, saved) and run one of 24 COP1 ops or 36 COP2 macro ops on `n` (default 2048) generated inputs, weighted to edges, through the EE core in use with interrupts off; `finite`: exponent 255 becomes 254 and denormals zero. COP1 record (6 words): inputs Fs, Ft, ACC, F0 and outputs Fd (ACC ops: ACC read back with MADD.S; C.cond: F0 unchanged) and FCR31 via CFC1. COP2 record (26 words): inputs VF1 (Fs), VF2 (Ft), ACC, VF3, I, Q and outputs VF3 (ACC ops: ACC read back with VMADDx), status, MAC, clip flags and Q via CFC2; ACC is set with VMULAw and the flags cleared before each case. After the last op RAM and registers are restored, records go to `file`, the log has one `EE optest: <op> <n> cases, hash <h>` line per op and `done ..., inputs intact` (else the borrowed RAM was written meanwhile). Compare two builds with `tests/zdxsv/ee_optest_cmp.py`. |

## Game investigation (EE recompiler)

Addresses and PCs are hexadecimal.

| Option | Use | Meaning |
|---|---|---|
| `ZDXSV_EE_PROBE=pc,pc,..` | diagnostic | Each time the EE reaches one of these PCs, log registers and memory to `<prefix>-<n>.txt`. |
| `ZDXSV_EE_PROBE_OUT=prefix` | diagnostic | Prefix of the probe and watch files. Default `eeprobe`. |
| `ZDXSV_EE_PROBE_MEM=addr` | diagnostic | Memory a probe logs: 48 bytes at `addr` (default `0xc22c98`), or `sp` for 128 bytes from the stack pointer. |
| `ZDXSV_EE_WATCH=addr[:len],..` | diagnostic | Log every EE store into these ranges (pc, address, value, ra, stack, GGPO frame) to `<prefix>-w<n>.txt`. |
| `ZDXSV_EE_PROFILE=prefix` | diagnostic | Windows. While rollback frames rerun, sample the CPU thread every ~0.2 ms: counts by EE block pc, VU1/VU0/IOP/VIF-unpack recompiled code, native function (DbgHelp, PDB next to the exe), its source line (`ln <file>:<line>`; inlined code counts at an outer line) and `cpuRegs.pc`, both (`at <pc> fn <function>`), EE stack page (`sp <sp / 4096> ee/other`: which game thread stack) and each return address found on the EE stack (`ra <addr>`: inclusive time of that call; a heuristic, stale stack words count too), native call chain (`ch <function> < <caller> < <its caller>`, by unwind data; stops at recompiled code, shown as `?!?`), rewritten to `<prefix>-<pid>.txt` every 2000 samples. |
| `ZDXSV_EE_CLAMP=addr,max[,lo,hi][;..]` | test | Keep a u16 at `addr` at or below `max`, only while it is in `lo..hi`. Used to shorten the sortie preparation (出撃準備) timer in rollback tests. A replay of such a battle needs the same value. |

## ARM64 cores

Read by ARM64 builds only. The cores: [arm64.md](arm64.md).

| Option | Use | Meaning |
|---|---|---|
| `PCSX2_VU1_JIT=0` | control | VU1 runs on the interpreter only. |
| `PCSX2_VU1_JIT=check` | test | Every VU1 trace is also run by the interpreter from the same start; VU1 registers and memory are compared, and every kept exit state with the real one (log `VU1 trace check`). Traces that start while an XGKICK transfer runs are not checked (the transfer cannot run twice). |
| `PCSX2_VU1_JIT_MAP=file` | diagnostic | Start address and kind of each emitted VU1 trace section (and the handler it calls), to attribute `sample` addresses in VU1 JIT code. |
| `PCSX2_ARM64_EE_NATIVE=0` | control | The EE recompiler calls the interpreter handler for every op (no native ops). |
| `PCSX2_EE_JIT_MAP=file` | diagnostic | Host address of each compiled EE instruction (pc, name, code word). |
