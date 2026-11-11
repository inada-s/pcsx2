# zdxsv rig tests

Scripts that run one or more PCSX2 instances with the game (and, for the lobby
tests, a local zdxsv server) and check the result from logs. Which test covers
which feature, its pass criterion and control: [Tests per feature](#tests-per-feature).
Each script's usage and knobs are in its header comment.

## Setup

- Windows (PowerShell 5.1), macOS or Linux; Python 3 (Pillow for windowed
  snapshots only). OS differences live in `zrig.py`. Key and mouse input (`PAUSE`, `KEYS`): `zrig.ctl` -> `pcsx2ctl.ps1` on Windows,
  `pcsx2ctl.js` (osascript, CGEvent) on macOS; the macOS shell running the rig needs Accessibility
  (rc 4 without it). Other window control (`replayui`, `settingsui`, `winshot`) is Windows only.
- A PCSX2 build of this repository (`bin/pcsx2-qtx64.exe`, or `PCSX2_EXE`):
  `build-local.sh` or MSBuild. Every build reads the test options.
- The game image (`ISO`). It is not part of this repository.
- The lobby tests (`m4*.py`, `stack.py`, `rbkprep.py`) need a zdxsv checkout
  with its binaries built in `$ZDXSV_DIR/bin` (`zdxsv.exe`, `zproxy.exe`) and Go to
  build `battlereg/`.
- The lobby trace (`TRACE`, an `EMU=1 m4z.py` OUT) and the rbk start states
  (`RBKSTATES`, made by `rbkprep.py`) are made on the machine; they are not
  part of this repository.
- Instance settings of the rig clients: merge `pcsx2.ini.part` into each
  `$RUN/pN/PCSX2/inis/PCSX2.ini` (`@IP@` = `IP`).
- The DEV9 adapter is off in every rig: `launch.py` writes `EthEnable` at each
  launch. Only clients that play as a PS2 (`m4z.py` without `EMU`, or
  `EXT_OFF`) get it on (`launch.py -Eth`).

## Settings

Each setting comes from the environment, else from `tests/zdxsv/local.env`
(`KEY=value` lines, ignored by git). A script that needs a missing setting
fails with exit 2 and names it.

| Setting | Used by | Value |
|---|---|---|
| `RUN` | all | work root: instance data dirs `$RUN/pN/PCSX2` |
| `ISO` | `launch.py` | the game image |
| `PCSX2_EXE` | `launch.py` | PCSX2 to test (Windows default `bin/pcsx2-qtx64.exe` of this tree; elsewhere required, a `.app` works) |
| `ZDXSV_DIR` | lobby tests | zdxsv checkout (`bin/zdxsv.exe`, `bin/zproxy.exe`) |
| `IP` | lobby tests | this machine's LAN IPv4 address, given to the game as the server |
| `DEV9` | `launch.py` | `1`: the game's net over the DEV9 adapter (empty `ZDXSV_SERVER_HOST`, as a PS2); else online without DEV9 to `IP` |
| `TRACE` | `m4.py`, `m4ggpo.py`, `rbkprep.py` | an `EMU=1 m4z.py` OUT dir: `lobby-trace.log` + `zdxsv.db`, made with the same `CARDS` |
| `RBKSTATES` | `rbk.py` | dir of `rbk-p1..p4.p2s` from `rbkprep.py` |
| `CARDS` | `m4z.py` | memory card names of p1..p4, space separated |
| `USERS` | `m4z.py` | zdxsv.db user ids of those cards, same order |
| `DZ` | `stack.py` | zdxsv binary for login + dnas (default the lobby's) |
| `ZDXSV_PY` | all | python with Pillow (default `python`) |
| `GOEXE` | `m4.py`, `zbincheck.py` | go (default `go`) |

Example `local.env`:

```
RUN=D:/zdxsv-run
ISO=D:/games/gvsz.iso
ZDXSV_DIR=D:/src/zdxsv
IP=192.168.0.10
TRACE=D:/zdxsv-run/trace
RBKSTATES=D:/zdxsv-run/prep
CARDS=card-p1 card-p2 card-p3 card-p4
USERS=AAAAAA BBBBBB CCCCCC DDDDDD
```

## Rules

- One rig at a time: every rig takes the lock in `zrig.take`
  (the instances share fixed ports).
- `launch.py` gives a client without `ZDXSV_GGPO` the value `0` (GGPO off,
  which is otherwise on by default); `ZDXSV_GGPO=default` leaves it on.
  Same for `ZDXSV_FAST_BOOT` (fast boot, on by default):
  `0` unless set, `ZDXSV_FAST_BOOT=default` leaves it on.
- `launch.py` runs `probelint.py` first and refuses a PCSX2 build older than
  its sources (an exe outside a PCSX2 tree, e.g. a copied `bin/`: no age check).
- Every script that starts PCSX2 sources the network pins (`launch.py`: `pcsx2_env`;
  `replayui.ps1`, `settingsui.ps1`: `localnet.ps1`, kept equal): fast login off and its API URL local, no-DEV9 names resolved to the box `IP`,
  so no run reaches the production server, also with a release exe.
- Run from the repository root, e.g. `OUT=D:/zdxsv-run/out python tests/zdxsv/rbk.py 2 1`.
  Exit 0 = every check passed; a failed check prints `FAIL` and its reason.
  `rbk.py` takes N and seed as arguments; an `N=` or `SEED=` in the environment
  that differs from them is refused (exit 2).
- Speed A/B: `OUT=<dir> python tests/zdxsv/benchab.py "<A env>" "<B env>" [R]` runs `rbk.py 2 1`
  (TRACE=0 LAT=40 JITTER=8) R times per side, alternating, on one build: a speed change is
  switched by its control env (e.g. `"" "ZDXSV_RERUN_REVERB=1"`). It prints each report part's
  mean and min-max per side, `differs` only when the ranges do not overlap. `R=0` re-summarizes
  the logs already in `OUT`.
- Pictures after rollbacks: `OUT=<dir> python tests/zdxsv/rerunpic.py ["<env>"]` runs a GGPO
  synctest with check=2 in the arcade battle, snapshots every shown frame and judges them with
  `tools/zdxsv/rerunpic.py` (exit 0 PASS). For changes to what rerun frames skip;
  `"ZDXSV_SINGLE_DRAW_LIST=0 ZDXSV_RERUN_TAIL=1"` is its FAIL control. ~4 min, ~0.5 GB of snapshots.
- Pictures after rollbacks by pixel color: `OUT=<dir> python tests/zdxsv/rerunpix.py ["<env>"]`: the same
  synctest with a 16-pixel watch instead of snapshots; PASS (exit 0) when every vsync shows a frame and
  no frame after a rollback repeats the one before. `"ZDXSV_SINGLE_DRAW_LIST=0 ZDXSV_RERUN_TAIL=1"` is its
  FAIL control.
- Input to picture: `OUT=<dir> python tests/zdxsv/inputpic.py ["<env>"]` presses down/up 8 times
  on the title main menu and watches one pixel of the highlighted item (`ZDXSV_INPUT_LATENCY`
  `pixel=`); prints each press' picture offset in vsyncs, exit 1 when a press has no change.
  Compare envs by the offsets. ~45 s. `tol=` (translucent battle HUD) has no rig test of its own.
- `lobbytest.py` is not a rig: a unit test of `pcsx2/Zdxsv/Lobby.cpp` built with
  MinGW `g++`. It needs no PCSX2 build, game, lock or server.

## Tests per feature

Feature names as in `docs/zdxsv/features.md`. **Control** is the variant that
must fail: it shows the test can detect the problem. "none": no automated test.
Results are not recorded here: a result names the commit it ran on.
Scripts are in `tests/zdxsv/` except `run.py` (repository root) and
`pwcheck.py`, `udprelay.py` (`tools/zdxsv/`).

### Online play

| Feature | Test | Control |
|---|---|---|
| Switch for all online battle extensions | `CLIENTS="1 3" EMU=1 EXT_OFF=1 m4z.py`: both clients log `all zdxsv extensions off`, send no platform info and no GGPO options, and join the battle server over TCP; both log in to the lobby again after the battle | without `EXT_OFF`: `sent platform info` |
| Teammate HP on the HUD | `allyhp.py` (`rbk.py 2 1`, RAM dumps of position 0): in battle the 4 max-HP loads of the HUD read the teammate HP field and it equals the teammate's HP, elsewhere they read max HP; at least 3 battle dumps | `CONTROL=1` (`ZDXSV_ALLY_HP=0`): FAIL |
| Team messages without a right stick | no test kept: a one-off check fed Start, D-pad, left and right stick through `Pad::SetControllerState` at boot of the Z game and read the pad's right stick; by hand: hold Start + D-pad in battle, the team message shows | setting `None` |
| GGPO lobby battle without the battle server | `m4z.py` `EMU=1 CLIENTS="1 3" GGPO=7101 GGPO_CLIENTS="1 3"` (stack `-v=2` battle log for the relay lines): `battle server answered on the EE x2, battle server unused` (no `[ROOM]` line), `join tcp x0`, `ggpo battle ends x1` | none (a build without it: `join tcp x2`) |
| Online without DEV9 | `m4z.py` `EMU=1 CLIENTS="1 3" GGPO=7101 GGPO_CLIENTS="1 3"` (DEV9 adapter off): `join tcp x0`, `platform info sent x2`, `p1/p3 ggpo session`, `ggpo battle ends x1: every recording past every peer's end msg`; without `GGPO`: `no DEV9: connect to <ip>:8210 refused`, `FAIL no battle`. Login: `nodev9.py` (fast login): the lobby ID selection shows, emulog `no DEV9` and `fast login: session` with the lobby on the server, no DNAS POST, no connect refused, TCP to the server only on the lobby and login API ports; fast login off (`ZDXSV_FAST_LOGIN=0`): `no DEV9: connect to <ip>:443 refused`, no DNAS POST | `nodev9.py` with `NODEV9=0 MAXS=90`: no ID selection, the network device pick shows the device as unavailable |
| A build from source connects to a local zdxsv | no test: by hand, `run.py rom` with no server settings against zdxsv `ZDXSV_AUTH_DEV=1 scripts/run-local.sh` reaches the dev sign-in page, signs in and logs in (emulog `names resolve to 127.0.0.1`, `fast login: by token`) | none |
| Default input settings: no upstream hotkeys, USB keyboard on port 1, one-time migration | no test: by hand, start the build with `-datapath <dir>` holding an older `PCSX2.ini` (upstream default hotkeys, `[USB1] Type = None`); after start the upstream hotkeys are gone, the `Zdxsv*` ones stay, `[USB1] Type = hidkbd`, `[Zdxsv] InputDefaultsVersion = 1`; a file already at version 1 is left alone. In-game chat, by hand: `SHOW=1 ROUTE=top,C,login,w300000 ROUTE_END=1 MAXS=520 boottime.py` (or `ZDXSV_FAST_BOOT=default ROUTE=w300000`, ○ presses to 戦場選択), enter a lobby, `pcsx2ctl.ps1 -AnyKey -Seq "A,H,J,K,Return,w1500,Return"`: the chat box shows `あhjk`, lobby.log has 0x6701 and the 0x6702 echo `82 a0 82 88 82 8a 82 8b` | `[USB1] Type = None`: no chat box, no 0x6701 (map cursor: only the pad d-pad moves it, hold arrows about 1 s per step) |
| USB keyboard sends the physical key (Windows) | no test: by hand, Windows with a Japanese keyboard layout, USB port 1 = `hidkbd`, in a lobby room type `@`, `:`, `^`, `A` then Enter twice: the chat line is `＠：＾あ` | the same build with `HostKeyNumber` returning 0: the line is `あ` (the three symbols dropped) |
| Fast login | `ROUTE=top LIMIT=200 boottime.py` (server with `/lbs/login`): user_id screen with no ログイン press, emulog `Zdxsv: fast login: session`, no DNAS POST in `dnas.log` | `ZDXSV_FAST_LOGIN=0 ROUTE=top LIMIT=200 MAXS=150 boottime.py` fails (stops at TOP) |
| Fast login at the game's reconnect | `m4z.py` `EMU=1 CLIENTS="1 3" GGPO=7101 GGPO_CLIENTS=1` (DEV9 adapter off; client 3 without GGPO cannot reach the battle server, the game gives up and opens its reconnect): `p3 battle unreachable without DEV9, back online after the game's reconnect` (a second `ZdxsvOnline: online` within 180 s), emulog `fast login: by token, from the game's login browser` | a build without it: one `ZdxsvOnline: online`, FAIL |
| Lobby connect after the first battle of a new user | no test: by hand, `run.py rom` signed in with an unused user ID, make the user, play one battle: emulog `result area cleared by the game, restored` and the game returns to the lobby | a build without it: `no DEV9: connect to 0.0.0.0:0 refused` |
| Fast boot | `fastboot.py`: run 1 (no boot state) logs `Zdxsv: fast boot: saving` and reaches the user_id screen with no input, run 2 logs `Zdxsv: fast boot: loading` and reaches it within 5 s of the VM start (`LIMIT`); each prints its time from the launch and from the VM start; run 3 (`OTHER_CARD`, default the second of `CARDS`) logs `Zdxsv: fast boot: saving` (no load of run 1's state) and leaves one boot state; run 4 (unformatted card) logs `Zdxsv: fast boot: stopped, memory card error` within 15 s; passes with the local zdxsv stack and with `LOBBY=fake` (`boot_lobby.py`: lobby API login and the lobby up to the user_id list only) | `ZDXSV_FAST_BOOT=0 MAXS=60 boottime.py` fails (no user_id screen), also with `LOBBY=fake`; run 3 on a build that keys the boot state on the adapter path only fails (`loading`) |
| Platform info `0x9950` to the lobby | `m4z.py` with samples of the lobby `/api/stat`: the lobby counts each client by platform | none |
| Battle over TCP to the battle server, as a PS2 | `m4.py`: 4 emulators from a fresh boot, all at once (fast boot through the user_id selection to 戦場選択 with `ZDXSV_FAST_BOOT_LOBBY`, fast login to the fake lobby's login API), fake lobby replaying `TRACE`, one battle. Variant `MASHP=1 SHOW=1`: time-up with side 1 ahead, all 4 report a loss | none |
| Playing as a console | `m4z.py` (default: 4 consoles, real lobby, DEV9 adapter on, all at once with fast boot to the login page, then login from the memory cards; `PAR=0`: one at a time from a fresh boot): every client takes the plain DEV9 path (adapter DHCP; no platform info, TLS, `bridge` or no-DEV9 line); the clients not behind zproxy (p2, p4) join the battle server over TCP; lobby 2 is 2v2, 4 results with the battle code of `battle_record` and the same frame count | none |
| A console behind a separate zproxy process | the same `m4z.py` run, `ZP="1 3"` (default): zproxy runs with `-upnp=false`, registers with the lobby, the PS2 connects to it and it joins the battle server over UDP (P2P on); no UPnP line in its log | none |
| First login from a blank memory card | `launch.py -Memcard <blank>`, then `drive.py fresh`, `register`, `newlogin` | none |
| First network setup without DEV9 (blank card, NoDev9 on) | `CARD=<blank> ROUTE=fresh,register,newlogin MAXS=280 LIMIT=300 boottime.py` (`SAVE_CARD=<path>` keeps the registered card): user_id screen; with `ROUTE_END=1`, `route done` (newlogin reaches 戦場選択: a new `user` row in the stack db); DNAS POSTs and `GET /CRS-top.jsp` in the stack logs | the build without these patches stays at net state 0x1a (no terms screen) |
| ネット終了 without DEV9 (NoDev9 on) | `ZDXSV_FAST_LOGIN=1 netend.py` (fast boot state): ID 1 → 戦場選択, ネット終了 → はい; `PASS`: the lobby log has `eventPeerLeave` within `LIMIT_END` s (default 5) of the `0x6006` answer; the game is back at the main menu | the build without the net end replies: `FAIL`, the connection stays open (the game hangs on 回線切断の準備); answering only `0x2f`: 12.5 s |
| Reset, then the lobby again without DEV9 (NoDev9 on) | `lobbyreset.py` (fast boot state, fast login to `boot_lobby.py`): online, the lobby stops, offline after about 2 min, F11 resets, fast boot runs again; `PASS`: online again and the new lobby's `user_id list sent` | a build that turns NoDev9 off at a reset: `FAIL online again`, fast boot gives up at the network menu |
| Fast login by token | `ZDXSV_FAST_LOGIN=1 tokenlogin.py` (server with token logins): stores a token for a db account, writes `inis/zdxsv_auth_token.txt`; `PASS`: emulog `fast login: by token`, its session is the token account's, the account got a temporary key; every rig with fast login on and no token file gets one for its card's account (`boottime.py`; `freshboot.py`: the stand-in lobby's token, the card's login key) | build without token login: `FAIL: emulog has no fast login: by token` |
| Sign-in and sign-out | `signin.py` (server with `/auth/`): fake OAuth provider on 127.0.0.1, p1 with `ZDXSV_AUTH_SIGNIN_TEST=1`, the script plays the browser; then p1 with `signout`; `PASS`: the token file holds a token the db stores for the fake user (no account made); after sign-out the file is gone, the db has no token for it, emulog `auth: signed out` with no error | build without sign-in: `FAIL: emulog has no auth: open` |
| Sign-in callback page | `authcallback.py` (no game, no server): PCSX2 without a game, `ZDXSV_AUTH_SIGNIN_TEST=1`, a stand-in token endpoint; the script sends the 127.0.0.1 callback with about 3 KB of headers, as a browser does; `PASS`: the answer is the whole `Signed in` page (no reset) and emulog has `auth: signed in` | a build that closes the callback socket with the request unread: `FAIL`, reset True |
| Sign-in at DNAS (no token) | `dnassignin.py` (server with `/auth/`, fast login, `ZDXSV_AUTH_AT_DNAS=1`): the fake provider's user is linked to a db account; the script browses `DELAY` s (default 90) after `auth: open`; `PASS`: emulog has the paused sign-in and `by token`, the session is the linked account's, the token file maps to it. `LIMIT` defaults to 600 (the pause counts). | build without it: `FAIL: paused sign-in False` (NET file key login) |
| Account pick (`ZdxsvAccount`) | `accountpick.py` (server taking `/lbs/login` `account`, fast login; `PICK` default 2): the two oldest accounts with users linked to one identity, token on the first, `ZdxsvAccount = PICK` in p1's ini; `PASS`: tokenlogin.py checks for the picked account. Settings page (local server with auth on, `ZDXSV_DISCORD_CLIENT_ID` set, a token in the data dir): `settingsui.ps1 -DataPath <dir> -Click zdxsvAccountAdd -Match zdxsvAccount`: a new last item picked, `ZdxsvAccount = N+1` in the ini, the server lists N+1; at 5 Add Account disabled; `-Select 'zdxsvAccountPick=^Account 2'`: `ZdxsvAccount = 2`, still picked at the next start | build without it: `FAIL: emulog session` (the first account's) |
| Login API down at DNAS (hold) | `apidown.py` (server with token logins, `LIMIT=600` default): token login against a relay on 127.0.0.1:9893 that starts listening `DELAY` s (default 60) after `fast login: by token`; `PASS`: tokenlogin.py checks, emulog `game held` and `login API answered` | build without the hold: `no answer from the login API, normal login`, no user_id screen |
| Slow login API under fast boot | `slowlogin.py` (fake login API behind a relay answering `DELAY` s late, default 2; fast boot to the user_id selection): `PASS`: user_id selection after one `fast login: by token`, no `the game left DNAS before the answer` | a build without the DNAS hold: the game leaves DNAS (its DNAS error) and logs in again each retry |
| No session at DNAS (back to the network menu) | `netmenu.py` (no server, `LIMIT=180` default): no token, `ZDXSV_FAST_LOGIN=1`, no sign-in at DNAS; the run connects again after the first return | the game reaches DNAS and goes back to the connection menu (net 3) twice, emulog has two `back to the network menu` lines | a build with the normal-login fallback stays in DNAS |
| Fast boot stops on a failed fast login | `netmenuboot.py` (no server): fast boot on, fast login with no token, no sign-in at DNAS; `PASS`: emulog has one `back to the network menu` line and `fast boot: stopped, fast login went back to the network menu`, nothing more for `SETTLE` (30) s | a build without the stop: fast boot connects again and again (`FAIL`, several lines) |
| Fast login again after ネット終了 | `ZDXSV_FAST_LOGIN=1 relogin.py`: netend.py, then 通信対戦 to DNAS by keys; `PASS`: two `fast login: session` lines, two user_id lists | build without the re-arm: one fast login |
| Menu routes keyed on game RAM (`drive.py top`, `login`, `idlogin`, `lobby2p`, the map walks of `map`, `map2p`, `remap2p`; `pine.py` `@probe=V`, `@probe!`, `Key~probe=V`, `Key~probe!`, `Key^probe>=V`, `Up|Down^probe=V`) | `ZDXSV_FAST_BOOT=0 ROUTE='top,C~step=4/6000' LIMIT=200 boottime.py`: PASS user_id screen; `m4z.py` (fast boot path) and `PAR=0 m4z.py` (network menu path) log in through `login` and enter Lobby 02 through `lobby2p`; 4-client `m4z.py` enters Lobby 06 through `map` (0x6305 00 06); `m4z.py` with `ENTRY2P_FAULT=1` enters Lobby 02 through `remap2p` from the top-left corner; a probe that never reaches its value raises `TimeoutError` with the value read | none |
| Probe trace for new route keys (`PINE_TRACE=<file>`, any rig) | `PINE_TRACE=<file> EMU=1 CLIENTS="1 3" m4z.py`: one line per route token and per probe change during waits (slot, frame, token, every PROBE value) | none |
| TLS to the zdxsv server | `m4z.py` with `EMU=1 CLIENTS="1 3" ZPROXY=`, `ZDXSV_TLS_TEST_PIN=1`, a `ZBIN` (and `DZ`) with TLS ports, `ZDXSV_TLS_CERT_FILE` / `ZDXSV_TLS_KEY_FILE` = the zdxsv test `server.crt` / `server.key`: every client logs a TLS connection through port 8221 and no TLS failure; the battle ends with results | the same run with `server-backup.crt` / `server-backup.key` (signed by the unpinned backup CA): every connection is refused, the OSD says so |
| Lobby connection kept while paused | `CLIENTS=1 EMU=1 SHOW=1 PAUSE=150 PAUSE_ONLY=1 m4z.py`: no `Recv Timeout` in the lobby log, >= 13 line checks answered while paused | `CENV=ZDXSV_PAUSE_LINE_CHECK=0`: `Recv Timeout` about 2 min into the pause |
| Pause menu | no committed test. By hand: `CLIENTS=1 EMU=1 SHOW=1 m4z.py`, Escape in the lobby: the log has `pause menu refused (lobby)`, no `(VMManager) Pausing...` | offline (no lobby connection), Escape opens the pause menu |
| Settings page | none (checked by hand) | none |
| Recommended-settings buttons | `settingsui.ps1 -DataPath <new dir> -Click zdxsvPresetHigh -Match 'zdxsvGgpoMinDelay$'`: the ini gets `upscale_multiplier = 3`, `ZdxsvGgpoMinDelay = 2`; `-Click zdxsvPresetLow`: `ZdxsvGgpoMinDelay = 3` | none |
| First-run wizard | none. By hand: start with `-datapath <new dir>`: the wizard shows the zdxsv texts and the zdxsv Account page; Next on the controller page with port 1 on the keyboard asks first; Sign in with Discord opens the browser and ends with Signed in on this PC | none |

### Input latency

| Feature | Test | Control |
|---|---|---|
| Low-latency present | `latency.py`: press-to-present time on the main menu | `latency.py -LowLatency 0`, or `ZDXSV_GAME_CRC=0` (not the Z game): the time is longer |
| Input latency measurement | it is the tool of the test above; `m4lat.py` uses it in a battle (`M4=m4ggpo.py`: over GGPO, frame pacing split by rollback) | none |
| Input to picture by pixel color | `inputpic.py` (title main menu, 8 presses): every press has a change, prints the offsets; `inputpic.py "ZDXSV_SINGLE_DRAW_LIST=0"`: 2 vsyncs later | `inputpic.py "ZDXSV_INPUT_LATENCY=...,pixel=40:10"` (not on the cursor): exit 1 |

### Boot to lobby

| Feature | Test | Control |
|---|---|---|
| Boot-to-lobby time measurement | `boottime.py`: with no input, the lobby's user_id selection screen within `LIMIT` s (default 5) of the VM start (emulog `Opening CDVD`; the launch script's start-up is not counted); passes with fast boot and its boot state (`fastboot.py` run 2) | `ZDXSV_FAST_BOOT=0 MAXS=60 boottime.py` fails (no user_id screen); `ZDXSV_FAST_BOOT=0 ROUTE=top,C boottime.py` reaches it but fails the limit |

### Rollback netcode (GGPO)

| Feature | Test | Control |
|---|---|---|
| GGPO library | unit: `GGPO.*` in `tests/ctest/core/ggpo_tests.cpp` | `GGPO.SyncTestCatchesStateOutsideSave` |
| GGPO pings answered on the network thread | `TRACE=0 rbk.py 2 1` x2: logged OSD pings 0-1 ms | a build that answers quality reports from the frame loop (`ggpo_idle`): pings 3-15 ms |
| GGPO test send delay flushed on the network thread | `GGPO_NETWORK_DELAY=30 TRACE=0 rbk.py 2 1` x2: p1 `peer report` `ping_1` max below 56 ms | a build that sends delayed packets only from the frame loop: max 63-66 ms |
| GGPO time sync spread over frames | `STALL=400 LAT=100 rbk.py 2 1`: peer 1 suspended 400 ms 8 times; peer 0 logs `sync N (max X ms)` with N >= 2, X < 25. `TURBO= STALL=400 rbk.py 2 1`: also OSD `P` at most 3 at the end | a build that sleeps all suggested frames at once: X about 33 (2 frames). A build whose waits the frame limiter takes back: `P` about 5 |
| Frames confirmed on this peer's own receipt | `TURBO= TRACE=0 LAT=50 DELAY=2 MAXWAIT=50 rbk.py 4 1`: PASS | `ggpo_set_disconnect_without_rollback` removed: about 1300 waits, FAIL MAXWAIT |
| Input sent before the rollback and the limiter sleep; limiter does not catch up; high priority | no pass criterion of its own: `TURBO= TRACE=0 LAT=50 DELAY=2 rbk.py 2 1`, compare the OSD `P` and the rollback depth (rollback frames / loads) between builds. Sync: `rbk.py 2 1`, `rbk.py 4 1` | input after the sleep: `P` and depth about 1 higher on the peer that is ahead |
| Relay forwarding on GGPO's network thread | `BLOCK=1-4 GGPO_LAT=10 GDELAY=auto m4ggpo.py`: clients 1 and 4 reach each other through a third peer and the battle ends clean. No test separates forwarding during a rollback from forwarding between frames | none |
| Frame time OSD stages | `TURBO= TRACE=0 LAT=50 DELAY=1 PCSX2_ENV=ZDXSV_FRAME_OSD=1 rbk.py 2 1`, then `frameosd.py --rollback $OUT/emulog-p*.txt`: stages add up to the frame, rollback stage > 0. Layout: screenshots, no pass criterion | same run without `LAT=50 DELAY=1`: `--rollback` fails (rollback stage 0) |
| GGPO session check | `MAXS=150 ENV0=ZDXSV_NET_SESSION=other rbk.py 2 1`: the peer refused logs `a peer is in another session` and closes `session mismatch`; the other `session mismatch` or `start timeout`. `PCSX2_ENV=ZDXSV_NET_SESSION=same rbk.py 2 1`: PASS | the build before: the first run plays the battle, PASS |
| Rollback budget in the lobby delay | `GDELAY=auto GGPO_LAT=50 m4ggpo.py`: `lobby delay 4` per peer, logged `rollback budget 0.00 (learned 1.00)`; `rbk.py 2 1`: each peer logs `rollback budget: fits` at the end | `PCSX2_ENV=ZDXSV_ROLLBACK_BUDGET=1`: `lobby delay 3`, the `lobby delay 4` checks FAIL |
| Delta save states | `deltatest.py` exit 0 (`result PASS`): the 2nd rerun of each window hashes like the 1st (same code cache) | `break=ee`: `result FAIL`, exit 1 |
| Faster rerun frames | `rerunpic.py "ZDXSV_SINGLE_DRAW_LIST=0"`: the pictures after rollbacks pass `rerunpic.py`; speed A/B with `benchab.py` | `rerunpic.py "ZDXSV_SINGLE_DRAW_LIST=0 ZDXSV_RERUN_TAIL=1"`: FAIL |
| Single draw list | `rerunpic.py`, `rerunpix.py`: PASS; `rbk.py 2 1`, `rbk.py 4 1`: PASS; `ENV0=ZDXSV_SINGLE_DRAW_LIST=0 rbk.py 2 1`, `rbk.py 4 2`: PASS; `menupic.py`: the title and network menus are not blank | `rerunpic.py "ZDXSV_SINGLE_DRAW_LIST=0 ZDXSV_RERUN_TAIL=1"` (the same tail without the patch): FAIL |
| Battle start and MS select load barriers | none | none |
| GGPO synctest in a running game | `deltatest.py -Var ZDXSV_GGPO -Spec start=9000,frames=3000,mask=fcff -Route arcade -End 12300`: 0 of 3000 frames mismatch in an arcade battle (`hash=pw`) | `control=input`: mismatches |
| Rerun keep | `ZDXSV_RERUN_KEEP=8afda0-8c95e0` (the same ranges in any game phase) with the synctest `deltatest.py -Var ZDXSV_GGPO -Spec start=9000,frames=3000,mask=fcff -Route arcade -End 12300`: 0 mismatches; `rbk.py 2 1`: PASS | `ZDXSV_RERUN_KEEP=afd000-afe000,6e4c00-6e5000,b13000-b6b000` (memory the game logic reads): mismatches |
| Rerun keep: attached parts | The same synctest logs `rerun keep draws attached parts`, 0 mismatches. No rig test for the picture (compared once against `ZDXSV_RERUN_EE_DRAW=1` pictures by hand) | `ZDXSV_RERUN_PARTS=0`: no log line |
| GGPO on by default for lobby battles | `CLIENTS="1 3" EMU=1 GGPO=7101 GDELAY=auto GGPO_DEFAULT=1 m4z.py`: client 1 has no `ZDXSV_GGPO`, logs the default options, GGPO battle with 0 mismatches | the same with `ZDXSV_GGPO=0` for client 1: client 3 cuts the battle connection, the checks fail |
| Battle UDP port setting | `CLIENTS="1 3" EMU=1 GGPO=7101 GDELAY=auto GGPO_DEFAULT=1 m4z.py` with no `ZdxsvGgpoPort` in client 1's ini: client 1 logs `battle port P picked and saved`, its ini gets `ZdxsvGgpoPort = P` (29700..29800), its GGPO session logs `port P` (check `GGPO port from the setting`) | none |
| Minimum input delay setting | `ZdxsvGgpoMinDelay = 4` in the `[DEV9/Eth]` section of both clients' ini, `CLIENTS="1 3" EMU=1 GGPO=7101 GDELAY=auto GGPO_DEFAULT=1 GMIN=3 m4z.py`: client 1 `lobby delay 4 ... min 4`, client 3 (`mindelay=3`) `lobby delay 3 ... min 3`, 0 mismatches | without the setting: `lobby delay 2 ... min 2` |
| GGPO battle without servers | `rbk.py 2 1` and `rbk.py 4 1`: random input, per frame, the x, y, z of every player and game RNG B agree on all peers (`pwcheck.py`). The start states come from `rbkprep.py`. Network variant: `LAT=30 JITTER=15 LOSS=0.05`. Also `run.py rbk_test_random` | `PS= rbk.py 4 1`: without the play-start barrier the peers desync |
| Every pad input reaches a GGPO battle | `rbk.py 2` + PINE presses on p1 (`pine.py` opcode 0x30, binds 9, 18-25): NET_TRACE `Z` lines of position 0, the synced (A, B) equals the game's own record (Start = A 0x8000 / B 0x8000, left stick = d-pad B bits, right stick = the same bits in A) | none |
| Session ends on a confirmed battle end | `LAT=100 rbk.py 2 1`: every peer logs `net end msg of frame F confirmed at frame C` with C >= F, `net battle end frames` = C + `ZDXSV_NET_TAIL`, 0 mismatches; `net end msg of frame F rolled back` when a rollback reran the sending frame. `COUNT=2`: one end message, after the second battle | none: no rig makes the game send the message only in a prediction |
| Clean end, tails differ | `LAT=100 LOSS=0.05 rbk.py 4 1` with `ENV0=ZDXSV_NET_TAIL=1` and `replay=DIR`: PASS, every peer logs `confirmed at frame C, leaving the peers` and `rbk exit (net battle end)`; `replay_check.py` on the 4 files PASS | the same run on a build before this: positions 1-3 stall 10 s after position 0 stops and end `disconnected` (FAIL no session end) |
| Clean end, peer left first | 1v1 `EMU=1 CLIENTS="1 3" GGPO=7101 GDELAY=auto m4z.py`: PASS `ggpo battle ends x1: every recording past every peer's end msg, 0 mismatches` (both clients log `net end msg of frame F confirmed`) | the same run on a build before this: the later client logs `peer disconnected` and ends `disconnected` at frame F (FAIL) |
| VM shutdown or reset ends the session | `rbk.py 2 1` with `replay=DIR` and `ENV0=ZDXSV_VM_TEST=1500:shutdown` (then `:reset`): position 0 logs `vm shutdown: session 1` and saves its replay, no `.replay-*.p2s` is left, the peer logs `peer disconnected`. Replay: `rplay.py` with `PCSX2_ENV=ZDXSV_VM_TEST=1400:shutdown`: no `zdxsv-replay*` file left in the cache | the build without the `VMManager.cpp` calls: shutdown leaves `.replay-*.p2s` and no replay; after a reset the old session goes on (hundreds of delta loads into the reset VM) to the battle end |
| A lobby battle that breaks after the start goes back to the lobby | 1v1 `EMU=1 CLIENTS="1 3" GGPO=7101 GDELAY=auto BREAK=3 BREAK_MODE=mute m4z.py`: client 3 drops every GGPO packet from GGPO frame 1500, its battle TCP stays up; client 1 logs `peer disconnected` and is back in the lobby (also `BREAK_MODE=shutdown`) | no test (the game ends the battle on its own once the battle socket goes back to it) |
| Pause and speed refused in a net battle | `TURBO= rbk.py 2 1` with `ENV0=ZDXSV_VM_TEST=1500:ui`: position 0 logs `refused during the battle` 4 times, `vm test load state: Not available during an online battle.` and `pause menu refused (battle)`, PASS | the build without the `VMManager.cpp` calls: `(VMManager) Pausing...`, the peer reports position 0 disconnected 10 s later, `no session end` FAIL |
| MTVU speedhack off while GGPO or a replay runs | `rbk.py 2 1` with `MTVU=1` (`vuThread = true` in the ini): every peer logs `MTVU speedhack off for this VM`, no `MTVU speedhack is enabled` warning (one per state save or load with MTVU on), 0 mismatches | the build without it: MTVU stays on, `MTVU speedhack is enabled` warnings |
| GGPO battle with the battle server and a fake lobby | `m4relay.py` with `DELAY`, `JITTER`, `LOSS`: the 4 battle results agree and every relay forwarded packets | none |
| GGPO battle from the battle info of the lobby | fake lobby: `m4ggpo.py`, 4 results agree and each client has 3 lobby peers. Real lobby: `EMU=1 GGPO=7101 m4z.py`, and 1v1 with `CLIENTS="1 3"` | `CLIENTS="1 3" GGPO_CLIENTS=1 m4z.py`: client 3 without GGPO, client 1 cuts the battle connection |
| Input delay from the ping test | 1v1 `m4z.py` with `GDELAY=auto`: log `lobby delay D`. `GMIN=3`: delay 3 | none |
| Ping test failure cuts the battle connection, OSD with the reason | 1v1 `m4z.py` with `BAD_SESSION=3`: both clients cut and return to the lobby. `GDELAY=auto BAD_SESSION=3 m4ggpo.py`: every client cuts and logs the `ZdxsvBattleCut` OSD "an opponent did not answer" | a build without the cut OSD: no `ZdxsvBattleCut` line |
| Connection diagram during the ping test | 1v1 `m4z.py` with `GDELAY=auto SHOW=1`, `winshot.ps1` of p1 1.5-6.9 s after its `battle info` line: two dots with bars between them, cyan at a local rtt (<= 30 ms) | a build without it: no diagram in the same shots |
| Sync-safe settings forced while the Z game runs | `syncset.py`: per setting, rbk N=2 with it changed on peer 0 only (`INI0`, `SPEED0`): 0 mismatches; `settingsui.ps1 -DataPath <dir> -Iso <Z game> -Page Advanced` (also Cheats, Game Fixes): forced items `enabled=False`, battle-only ones (Emulation page) enabled in the lobby | the same with `ZDXSV_SYNC_FORCE=0`: EE FPU or FPU-div rounding desyncs, fast CDVD and the NTSC frame rate lose the session end, extra memory cannot load the rbk states; the other settings pass in this 30 s battle. Greyed-out settings: `ZDXSV_SYNC_FORCE=0`, all enabled; greyed-out during a battle not checked |
| Z game patches built in | none | none |
| No patches.zip, no warning | `rbk.py 2 1` with no `bin/resources/patches.zip`: no emulog has `PatchesZipOpenWarning`, each has `Found 1 game patches in zdxsv built-in patches.` Not tested: `ZDXSV_EXTENSIONS=0` | the same run on a build without the hook: `PatchesZipOpenWarning` in each emulog |
| Operations refused while online | `onlinelimits.py state`: at the lobby, PINE save and load are refused, no state file. `onlinelimits.py hotkeys`: F1 (save state), F9 (software renderer), Shift+F8 (GS dump) refused, no state file. `onlinelimits.py system`: reset and shutdown hotkeys refused, p1 runs on; the test reset of `ZDXSV_VM_TEST=...:reset` is not refused. `onlinelimits.py breakpoints`: a test breakpoint set at boot is cleared at the online switch; in a battle: `PCSX2_ENV=ZDXSV_TEST_BREAKPOINT=1fffff00 rbk.py 2 1` PASS with `debugger breakpoints cleared`. Not tested: the menus, disc and memory card changes, GS setting changes, the quit question in a battle; by hand in the lobby: System → Video Capture, Debug → Open Debugger: OSD `Not available while online.`, log `ZdxsvOnline: refused while online` | `NOLOBBY=1` with each case: offline, the save works, FAIL |
| PCSX2 features limited while online | `onlinelimits.py pine`: at the lobby, PINE requests fail (`PINE requests refused`). `onlinelimits.py forced`: manual hardware fixes and texture dumping set in the ini are forced off. Not tested: macros | `NOLOBBY=1` with each case: offline, PINE answers, nothing forced, FAIL |
| Sync fingerprint | 1v1 `EMU=1 CLIENTS="1 3" GGPO=7101 SYNC_SALT=3 m4z.py`: the lobby gets 2 different `sync:` fingerprints, both clients enter (0x640E), no battle starts | the same without `SYNC_SALT`: a GGPO battle |
| Match report: timesync, inputs, fps, loss | `GDELAY=auto GGPO_LAT=10 MAXS=700 m4ggpo.py`: each client with a GGPO session logs `peer report` with `timesync=`, `input_blocks=`, `fps_history=` and `loss_<peer>=` per peer. Encoding: `lobbytest.py` checks `total_timesync`, `input_block_count`, `fps_history`, `recv_loss`/`send_loss` in the report JSON against `matching_report.json` | the build without it: the `m4ggpo.py` peer report check FAILs; a renamed report key: the lobbytest report check FAILs |
| Match report sent to the lobby | `REPORT=1` on the 1v1 or the cut case: the lobby logs 2 reports with the battle code and the expected result | none |
| Match report: who left, ping | `LAT=100 rbk.py 2 1`: each client logs `peer report` with `ping_<peer>` min/median/max near 2 x LAT. A client killed mid-battle: the other logs `disconnected_positions=<its position>` (rbk FAILs). Encoding: `lobbytest.py` checks `ping_min_ms`/`ping_median_ms`/`ping_max_ms` and `disconnected_peer_ids` in the report JSON against `matching_report.json` | the build without it: no `peer report` line; a renamed report key: the lobbytest report check FAILs |
| Relay servers | `RELAY=1 GGPO_LAT=100 GDELAY=auto m4ggpo.py`: every path `relay 0`, delay 2. `RELAY=4` (relay_0..3, nothing at 0 and 1): every path `relay 2`. With `RELAY_ALT=1` (relay answers from another address than advertised): the same, plus `answers from 127.0.0.1:8203`. Real lobby: the same options on 1v1 `m4z.py` | the same without `RELAY`: every path `direct`, delay 7. `RELAY_ALT=1` on a build that requires the advertised source: every path `direct` |
| Peer without an address in the battle info | `NOADDR=4 GDELAY=auto m4ggpo.py`: every client `net player`, 3 lobby peers; clients 1-3 reach client 4 at the source of its pings | the same on a build that drops pings from unlisted sources: cut |
| Path through a relaying peer | `BLOCK=1-4 GGPO_LAT=10 GDELAY=auto m4ggpo.py` (fake lobby: no path between clients 1 and 4): clients 1 and 4 reach each other through a third peer (`path ... peer`, peer `(relay)`), their lobby delay from that path, every other path direct (and m4.py's checks) | the checks without `BLOCK` on the same logs: clients 1 and 4 FAIL `paths direct x3` |
| Unreachable peer cuts the battle connection | `UNREACH=1 GGPO_LAT=10 GDELAY=auto m4ggpo.py` (fake lobby: no path from client 1 to anyone, not even through a peer): client 1 gets no answer, every other client gets rtt -1 from client 1 only, no drops; every client cuts with the `ZdxsvBattleCut` "an opponent did not answer" OSD, no GGPO session, the game gives up the battle connection | the checks without `UNREACH` on the same logs: every client FAILs `ggpo session` |
| Cut player refuses GGPO | 1v1 `EMU=1 CLIENTS="1 3" GGPO=7101 DROP_SYNC=3 m4z.py` (4 players: without `CLIENTS`): client 3 cuts and logs `cut refusal: refused the GGPO session`, every other client logs `a peer is in another session` within 3 s of `net armed`, all back in the lobby | a build without the refusal: client 1 logs `net peers not synchronized in 10 s`, the checks fail |
| Dual-stack peers | unit: `GGPO.UdpDualStack`: the GGPO socket sends and receives over IPv6 (skipped on a host without IPv6). The choice between the candidates has no test | none |
| P2P connectivity test | 1v1 `m4z.py` with `NAT=open`: one `udp test: nat=open` line per client. `lobbytest.py`: with a udptest `Hello` `nat=open`, without it `nat=cone`, a failed request `nat=open`; fake outside STUN servers with different mappings give `nat=symmetric` | the `lobbytest.py` checks with the udptest step off: the `Hello` and `nat=cone` checks FAIL; `ZDXSV_NAT_STUN=none`: the same fake lobby gives `nat=cone` |
| UPnP mapping of the GGPO port | 1v1 `m4z.py`, `CENV="ZDXSV_STUN_PORT=8299 ZDXSV_GGPO=net=1,lobby=1 ZDXSV_UPNP_ANY_IGD=1"` (closes the test): `zdxsv UPnP: UDP port N mapped`, a second `udp test` line, `upnpc -l` lists `PCSX2 zdxsv` | without `ZDXSV_UPNP_ANY_IGD` on a router reporting WAN `0.0.0.0`: `Internet Gateway not found: error 2`, no mapping |
| "UPnP Now" button on Settings → zdxsv | ini `[DEV9/Eth] ZdxsvGgpoPort = N`, `ZDXSV_UPNP_ANY_IGD=1 settingsui.ps1 -DataPath <dir> -Click zdxsvUPnPNow -Wait 15 -Match zdxsvUPnP -Quit`: `zdxsvUPnPResult 'UDP port N opened'`, `upnpc -l` lists `PCSX2 zdxsv` while it runs, `quit rc=0`, then none | without `-Quit` (killed): the mapping stays (`upnpc -d N UDP`); without `ZDXSV_UPNP_ANY_IGD` on a WAN `0.0.0.0` router: `Internet Gateway not found: error 2`; port 0: `Set a battle UDP port first` |
| HTTPS latency test | 1v1 `m4z.py` with `HTTPS=1`: one test per client, its platform info in `lobby.log` has `asia-northeast1` | `HTTPS_OFF=3`: that client's platform info has no region |
| `machine_id` in the platform info | 1v1 `m4z.py` with `EMU=1`: each client logs `sent platform info (N bytes, machine_id <16 hex digits>)`, the same id on both clients of one PC and across runs | a build without it: the line has no `machine_id` |
| OSD message when the lobby refuses this PCSX2 as outdated | no committed test. By hand: 1v1 `m4z.py` with `EMU=1` on a build with a `zdxsv-X.Y.Z` tag and the lobby's `m_server_setting` `min_pcsx2_version` above it: the lobby logs `outdated client refused`, the client `zdxsv lobby refused this PCSX2 as outdated` | `min_pcsx2_version` below the build's version: login goes on, no such client line |
| Lobby network waits and ping test RTT | `lobbytest.py`: unit test of `Zdxsv/Lobby.cpp` with a fake STUN and a fake peer that answers from another port, and one whose every 4th pong is 400 ms late (rtt < 50 ms) | the pong-source case FAILs on a build that counts only pongs from the pinged address; the late-pong case FAILs on a build with the mean RTT |
| Battle info, match report and STUN Ping/Pong as JSON | `lobbytest.py`: decodes `testdata/wire/battle_info.json` (zdxsv's exact output) and `battle_info_full.json` and checks the limits; the encoded report equals `matching_report.json`, the Ping equals `ping.json`, `pong.json` decodes (the fixtures zdxsv's own tests check) | one report key or the Pong `public_addr` key renamed: the report, Pong and UDP test checks FAIL |
| Match report round data | 1v1 `EMU=1 CLIENTS="1 3" GGPO=7101 UPLOAD=1 m4z.py` with a `ZBIN` that knows `round_data`: both match reports carry the same `round_data`, one `used_ms` per player per round | the same with a client without round data: FAIL |
| Network status OSD | `EMU=1 GGPO=7101 OSD=1 m4z.py`: the logged OSD text of each client has its delay and every opponent's user id, name, pilot name and ping; names and pilot names equal the lobby db (`osdname.py`) (the drawn panel: no committed test) | none |
| Network OSD on disconnect | `rbk.py 2 1` with `ENV0=ZDXSV_VM_TEST=1500:shutdown` (also with `GGPO=net=1,players=2,delay=2,osd=0`): the peer logs `peer disconnected`, then `osd held 10 s: ... 1P ... | Disconnected`\| Disconnected`; `PCSX2_ENV_P4=ZDXSV_VM_TEST=1500:shutdown GDELAY=1 m4ggpo.py`: clients 1-3 log `osd held` with `4P ... \| Disconnected` and `osd hold over` 10 s later | the build without it: `peer disconnected` only |
| OSD Lag meter idle on a good link | `TURBO= TRACE=0 MAXLAG=0 rbk.py 2 1` and `TURBO= TRACE=0 MAXLAG=1 LAT=15 rbk.py 2 1` (the relay adds jitter): no `osd lag` above n in the 2nd half of the battle | a build that expects only one pending input per frame of the ping: the meter stays at 2 |
| GGPO packet loss not counted for the sync phase | `TURBO= TRACE=0 MAXLAG=0 MAXLOSS=0 rbk.py 2 1`: every `peer report` loss count 0; `LAT=5 LOSS=0.05` with the same: FAIL MAXLOSS (real loss is still counted) | a build that leaves the receive sequence at 0 when sync completes: loss 10-20, FAIL MAXLOSS |
| In-game button settings in GGPO battles | `padbind.py` (`rbk.py 2 1` from states where p1's buttons all have other actions, p3 default): `rbk.py` PASS; on each peer every own synced A = the A its settings give for its B, and = the game's own record where B pairs | the build with the fixed default bind table: p1 A unlike its settings on most press frames and unlike the game's own record, FAIL; p3 passes |
| A button set to no action in the in-game button settings | no test: by hand, set one button to no action in the game's settings, save to the card, play a GGPO battle: that button does nothing and the other buttons keep their settings | a build where the settings table has 8 actions: every button has its default action |
| UDP sockets ignore ICMP port unreachable on Windows | unit: `GGPO.UdpReadsPastPortUnreachable`: after a send to a closed port, the next poll still reads the datagram behind the error | the same test with the `SIO_UDP_CONNRESET` call removed from `udp.cpp`: 0 datagrams read |

### Replays

| Feature | Test | Control |
|---|---|---|
| Save a GGPO battle as a replay | `rbk.py 2 1` with `replay=DIR`, then `replay_check.py` on the file of every peer: same inputs on the common frames. Real lobby: 1v1 `m4z.py`, then `replay_check.py` | a copy with 1 input byte changed fails at that frame |
| Upload a lobby battle's replay | `UPLOAD=1 CLIENTS="1 3" EMU=1 GGPO=7101 GDELAY=auto m4z.py` (`upload=` to the local lobby's `/lbs/replay_upload`): the stored `.pb` = one client's, one upload ok + the rest 409, every record's `replay_url` = the stored url. The default URL and the DNS gate: none (they need the public server) | the same run without `UPLOAD=1` (host entries, no `upload=`): no `replay upload` line |
| Play a replay | `rplay.py` against the trace of the live battle: player work equal on every frame | a copy with changed buttons fails from the changed frame |
| Replay that does not start | `BAD=1 FILE=<random bytes> rplay.py`: logs `replay: not started` (the OSD text) | the build without it: only `not a replay file`, FAIL |
| Play a replay by URL or battle code | `UPLOAD=1 REPLAY_API=4 PWTRACE=1 CLIENTS="1 3" EMU=1 GGPO=7101 GDELAY=auto m4z.py`: the API's `replay_url` = the stored url, player work + rng = the players'. Without a lobby: `API=1 FILE=<the battle's .pb> rplay.py <its traces>`; gzip encoded as the bucket serves it: `GZIP=1` too | `API=1 FILE=<another battle's .pb> rplay.py` with the same traces fails; `GZIP=1` before the gzip decoding: `not a replay file` |
| Common state | no script: serve a state over HTTP, `rplay.py` with `PCSX2_ENV=ZDXSV_REPLAY_STATE=http://127.0.0.1:<port>/<state>` once, cut the cached `zdxsv-common-*.p2s` it logged to 5000 bytes, run again: `cached copy load failed`, the download, `loaded`, PASS | the build without it: `load failed` on every try, FAIL |
| Seek | `rplay.py` with `ZDXSV_REPLAY_SEEK=at:to,...`: the predicted key is loaded and player work equals live after the seeks. Hotkeys: `WINDOW=1 KEYS=...` | `ZDXSV_REPLAY_KEY_NOHLE=1` with the same seeks fails |
| Point of view | `POV=1 rplay.py` on the position-0 file: `net armed ..., position 1` + pwcheck; `PCSX2_ENV=ZDXSV_REPLAY_POV_AT=2500:1,3500:0`: new position's battle start, seek, state hashes equal | without `POV` the game arms as the recorder (position 0) |
| Control bar | `rplay.py` with `WINDOW=1 KEYS="...bar:NAME..."` (real mouse through `pcsx2ctl.ps1` / `pcsx2ctl.js`): the logged seek frame is the one computed from the click position | none |
| Key display | `keycheck.py`: every logged key history equals the inputs in the file up to that frame | a copy with 1 input bit flipped fails for the histories that reach that frame |
| Live spectating | `m4z.py` `LIVE=2 PWTRACE=1`: the spectator closes at the uplink's saved frame count, checks more than 0 state hashes and 0 differ, player work + rng = the players' | a receiver that ignores the lobby's close push (it has no frame_bytes) stalls 60 frames before the end |
| Live round results | `LIVE=2 LIVE_AT=150 PWTRACE=1 CLIENTS="1 3" GGPO_CLIENTS="1 3" EMU=1 GGPO=7101 GDELAY=auto m4z.py` (lobby from zdxsv with `round_ack`): the spectator, started 150 s after the uplink, has round results at its open, gets every one the uplink sent, and none differs from its own playback | a lobby that does not relay `round_data`: the spectator logs no `round result ... from the stream` |
| Live speed trim | `LIVE=2 LIVE_SPEED=1.02 CLIENTS="1 3" EMU=1 GGPO=7101 GDELAY=auto m4z.py`, the spectator's `live: pace` log lines after the battle load (read, no scripted check): gap 28-32, at most 1 more wait | the same with `ZDXSV_LIVE_PACING=0`: the gap runs down to the newest frame |
| Live wait without blocking | `LIVE=2 LIVE_STALL=20 CLIENTS="1 3" GGPO_CLIENTS="1 3" EMU=1 GGPO=7101 GDELAY=auto m4z.py`: the lobby is suspended, the spectator exits within 5 s of its window's close | a build that waits on the CPU thread: exit only after the 30 s stream loss |
| Replay window | `replayui.ps1 -DataPath <instance> -Tab Local/Remote/Live/Favorites -Row <code>` (UI Automation; Remote/Live against a local zdxsv with `ZDXSV_LOBBY_API_ADDR`, Live during `m4z.py`): the instance emulog logs `replay <source>` and the common start of the picked point of view; the lobby logs `live subscribe`. Remote `-Set 'Edit:User ID=<id>;Combo:Players=4 players;Check:Rated only;Check:Oldest first'`: the lobby gets `user_id=<id>&players=4&aggregate=1&reverse=1`; Remote `-Set 'Combo:Mobile suit=<MS>'`: only battles where a player's `used_ms_list` holds the MS id + 1 (another MS: `No replays found.`); `-Button 'Add to Favorites'`, then `-Tab Favorites -Row <code>`: the ini holds `ZdxsvReplayFavorites`, the emulog `replay <API>/lbs/replay?battle_code=<code>`; a played Remote or Favorites row: emulog `replay played <API>/lbs/replay_played?battle_code=<code>: status 200`, the next Remote search shows the lobby's `play_count` one higher under Views | the same `.pb` through `ZDXSV_REPLAY` logs the same lines and state hashes, and no `replay played` |
| Return to the game after a replay | `replayui.ps1 -Boot <Z game iso> -Tab Local -Row <code>` with `ZDXSV_REPLAY_EXIT=1`: the emulog logs `replay return: <data>/replays/return.p2s`, the game runs on from that state and the window is open again (UIA) | setting off: the VM stays stopped, no `replay return` |
| Second replay in one process | `replayui.ps1 -Tab Local -Row <code A> -Row2 <code B> -Wait 75 -Wait2 70` with `ZDXSV_REPLAY_TAKEOVER=2000:replay` (A is taken over, then B starts while A runs): after B's `replay <file>: recorded at` line the emulog logs `replay skip MS selection: briefing at frame` and `replay key` lines | a `PlayReset` that keeps the takeover state: B logs `replay ended first`, no briefing |
| Live auto-next | `BATTLES=2 LIVE=2 LIVE_NEXT=1 PWTRACE=1 CLIENTS="1 3" EMU=1 GGPO=7101 GDELAY=auto m4z.py`: the spectator waits at the end of battle 1, moves on to battle 2; both streams close at the uplink's saved frame count, battle 2 player work + rng = the players' | `LIVE_NEXT=0`: the spectator exits after battle 1 (not run) |
| Live auto-next skip | `livenext.py`: a spectator of a port with no lobby logs the skip and its cause, pauses and keeps waiting | a build without the skip logs `not a replay file` and does not pause |
| Rounds only | no committed script. By hand: play a 3-game lobby `.pb` headless (`launch.py` instance 5 with `ZDXSV_REPLAY=<file> ZDXSV_REPLAY_EXIT=1 ZDXSV_REPLAY_TURBO=1 ZDXSV_REPLAY_SEEK=8000:12000,13000:12999`): emulog `rounds shown with skipping:` 3 spans, `outside the rounds, skipping to round` before rounds 1-3, the seek to 12000 done at the end of round 2, `past the last round`, `state check: ... 0 differ`. A file without play starts: `rplay.py` with `SKIP_MS=1`: the logged briefing frame equals the one in the live trace | `ZDXSV_REPLAY_SKIP_MS=0`: no skip lines, the selection and rematch screens play |
| Round jump | `rplay.py` with `SKIP_MS=1 PCSX2_ENV=ZDXSV_REPLAY_ROUND_AT=1000:2,17000:1,6000:3`: every logged load end is 1 frame after a live trace `L` line leaving state 8 (same vsync), the rounds start at the predicted loads, player work equals live. Keys and bar: `WINDOW=1 KEYS="10:Shift+PageDown;40:bar:show,w600,bar:nextround"` | none |
| Round mobile suits | `rplay.py` with `WINDOW=1 KEYS="20:bar:show,w600,bar:results,w1200"` on a file whose `round_data` has `used_ms`: emulog `replay bar: round MS R1 ...` names those mobile suits, the shown team first | none |
| Takeover | `rplay.py` on a 2-player lobby battle `.pb` with `SKIP_MS=1 PCSX2_ENV="ZDXSV_REPLAY_TAKEOVER=6000:replay ZDXSV_REPLAY_TAKEOVER_RETRY=9000"`: the own position packed by the takeover (pad + kind-3 msgs, delay 2) plays to the end, retry goes back to 6000, player work + rng = the players' | `ZDXSV_REPLAY_TAKEOVER=6000:rand`: FAIL. Countdown panel with a real pad: not run |
| Frame step, playback speed, pad controls | no test. By hand: pause, pad Left / Right step one frame (emulog `seek requested`, paused again), Up / Down log `replay speed N %` | none |
| Operations refused during a replay | no test. By hand: during a replay a state save or load hotkey logs `refused during a replay`, the OSD says so; seeks still load their keys | none |
| Four-screen replay | `FOUR=1 rplay.py`: guest trace pwcheck + host `spread` lines <= 4 frames | `PCSX2_ENV=ZDXSV_REPLAY_SYNC=0`: the spread FAILs |

### Game window

| Feature | Test | Control |
|---|---|---|
| No save state on shutdown | no test. By hand: close the window during a game: the dialog has no `Save State For Resume`, and no resume state is written to the save state folder; `ZdxsvFeatureLimits = false`: as upstream | none |
| No game properties | no test. By hand: the System menu, the game list context menu and Big Picture have no game properties entry and a saved game settings ini is not loaded (emulog has no `Loading game settings from`); `ZdxsvFeatureLimits = false`: as upstream | none |
| Mouse wheel volume | no test. By hand: wheel over the game screen, OSD `Volume: N%` steps by 5 and the status bar volume follows; setting off or the wheel bound: no change | none |

### ARM64 builds

| Feature | Test | Control |
|---|---|---|
| EE and IOP recompilers with the x86 timing | the same replay with `ZDXSV_CYCLE_LOG` on an ARM64 and an x86_64 build: same cycle lines | none |
| VU1 trace recompiler | a replay with `PCSX2_VU1_JIT=check`: `0 differ`; `ZDXSV_VU1_OPTEST` vs the x86 build: `0 differ` | none |
| FPU and VU math as x86 | `ZDXSV_VU1_OPTEST` and `ZDXSV_EE_OPTEST` on an ARM64 and an x86_64 build, then `vu1_optest_cmp.py` / `ee_optest_cmp.py`: no op differs | none |
| EE FPU interpreter as the recompiler | none | none |

### Releases and tools

| Feature | Test | Control |
|---|---|---|
| Self-update from the GitHub releases of this fork | `updatecheck.py --exe <build at a zdxsv-X.Y.Z tag> --inis <data dir>/PCSX2/inis`: a local release list offers `zdxsv-999.0.0`, the startup check fetches it once and logs `Update needed.` | `--offer zdxsv-0.0.0`: `No update needed.` An untagged build fetches nothing |
| Crash reports | none: needs a Sentry account. By hand: run with `ZDXSV_SENTRY_DSN=<dsn> ZDXSV_CRASH_REPORT_TEST=1`; PCSX2 crashes and the issue appears in Sentry with `PCSX2.ini` and `emulog-tail.txt` (the log's last lines) attached | none |
| Release workflow | none | none |
| Windows CI build | the workflow itself; it also runs the unit tests | none |
| PINE opcodes `0x30` to `0x32` | no test of its own; the rig scripts drive the emulator through them (`pine.py`) | none |
| Fake lobby log lines from concurrent connections | `lograce.py`: 4 threads log 3000 `0x6138` and `BATTLE RESULT` lines each through `fake_lobby.log`, none lost or glued | `fake_lobby.log` as an unlocked `print()`: lines glued and lost, FAIL |
| EE probe, watch and rerun profile | `probelint.py`: refuses a bad or stale probe list | none |
| `run.py` | `rbk_test` ends with the sync check of `pwcheck.py` | none |
